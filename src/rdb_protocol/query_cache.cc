// Copyright 2010-2015 RethinkDB, all rights reserved.
#include "rdb_protocol/query_cache.hpp"

#include "rdb_protocol/env.hpp"
#include "rdb_protocol/response.hpp"
#include "rdb_protocol/term_walker.hpp"

namespace ql {

query_cache_t::query_cache_t(
            rdb_context_t *_rdb_ctx,
            ip_and_port_t _client_addr_port,
            return_empty_normal_batches_t _return_empty_normal_batches) :
        rdb_ctx(_rdb_ctx),
        client_addr_port(_client_addr_port),
        return_empty_normal_batches(_return_empty_normal_batches),
        next_query_id(0),
        oldest_outstanding_query_id(0) {
    auto res = rdb_ctx->get_query_caches_for_this_thread()->insert(this);
    guarantee(res.second);
}

query_cache_t::~query_cache_t() {
    size_t res = rdb_ctx->get_query_caches_for_this_thread()->erase(this);
    guarantee(res == 1);
}

query_cache_t::const_iterator query_cache_t::begin() const {
    return queries.begin();
}

query_cache_t::const_iterator query_cache_t::end() const {
    return queries.end();
}

scoped_ptr_t<query_cache_t::ref_t> query_cache_t::create(query_params_t *query_params,
                                                         signal_t *interruptor) {
    guarantee(this == query_params->query_cache);
    query_params->maybe_release_query_id();
    if (queries.find(query_params->token) != queries.end()) {
        throw bt_exc_t(Response::CLIENT_ERROR,
            strprintf("ERROR: duplicate token %" PRIi64, query_params->token),
            backtrace_registry_t::EMPTY_BACKTRACE);
    }

    backtrace_registry_t bt_reg;
    counted_t<const term_t> root_term;
    counted_t<term_storage_t> term_storage;
    scoped_ptr_t<global_optargs_t> global_optargs;
    try {
        term_storage = make_counted<term_storage_t>(
            std::move(*query_params->root_term_json))
        global_optargs = make_scoped<global_optargs_t>(
            std::move(*query_params->global_optargs_json));
        preprocess_term_tree(term_storage->root_term(), &bt_reg);

        compile_env_t compile_env((var_visibility_t()));
        root_term = compile_term(&compile_env, term_storage->root_term());
    } catch (const exc_t &e) {
        throw bt_exc_t(Response::COMPILE_ERROR, e.what(),
                       bt_reg.datum_backtrace(e));
    } catch (const datum_exc_t &e) {
        throw bt_exc_t(Response::COMPILE_ERROR, e.what(),
                       backtrace_registry_t::EMPTY_BACKTRACE);
    }

    scoped_ptr_t<entry_t> entry(new entry_t(*query_params,
                                            std::move(bt_reg),
                                            std::move(term_storage),
                                            std::move(global_optargs),
                                            std::move(root_term)));
    scoped_ptr_t<ref_t> ref(new ref_t(this,
                                      query_params->token,
                                      entry.get(),
                                      interruptor));
    auto insert_res = queries.insert(std::make_pair(query_params->token,
                                                    std::move(entry)));
    guarantee(insert_res.second);
    return ref;
}

scoped_ptr_t<query_cache_t::ref_t> query_cache_t::get(query_params_t *query_params,
                                                      signal_t *interruptor) {
    guarantee(this == query_params->query_cache);
    query_params->maybe_release_query_id();
    auto it = queries.find(query_params->token);
    if (it == queries.end()) {
        throw bt_exc_t(Response::CLIENT_ERROR,
            strprintf("Token %" PRIi64 " not in stream cache.", query_params->token),
            backtrace_registry_t::EMPTY_BACKTRACE);
    }

    return scoped_ptr_t<ref_t>(new ref_t(this,
                                         query_params->token,
                                         it->second.get(),
                                         interruptor));
}

void query_cache_t::noreply_wait(const query_params_t &query_params,
                                 signal_t *interruptor) {
    guarantee(this == query_params.query_cache);
    auto it = queries.find(query_params.token);
    if (it != queries.end()) {
        throw bt_exc_t(Response::CLIENT_ERROR,
            strprintf("ERROR: duplicate token %" PRIi64, query_params.token),
            backtrace_registry_t::EMPTY_BACKTRACE);
    }

    oldest_outstanding_query_id.get_watchable()->run_until_satisfied(
        [&query_params](uint64_t oldest_id_value) -> bool {
            return oldest_id_value == query_params.id.value();
        }, interruptor);
}

void query_cache_t::terminate_query(const query_params_t &query_params) {
    guarantee(this == query_params.query_cache);
    assert_thread();
    auto entry_it = queries.find(query_params.token);
    if (entry_it != queries.end()) {
        terminate_internal(entry_it->second.get());
    }
}

void query_cache_t::terminate_internal(query_cache_t::entry_t *entry) {
    if (entry->state == entry_t::state_t::START ||
        entry->state == entry_t::state_t::STREAM) {
        entry->state = entry_t::state_t::DONE;
    }
    entry->persistent_interruptor.pulse_if_not_already_pulsed();
}

query_cache_t::ref_t::ref_t(query_cache_t *_query_cache,
                            int64_t _token,
                            query_cache_t::entry_t *_entry,
                            signal_t *interruptor) :
        entry(_entry),
        token(_token),
        trace(maybe_make_profile_trace(entry->profile)),
        query_cache(_query_cache),
        drainer_lock(&entry->drainer),
        combined_interruptor(interruptor, &entry->persistent_interruptor),
        mutex_lock(&entry->mutex) {
    wait_interruptible(mutex_lock.acq_signal(), interruptor);
}

void query_cache_t::async_destroy_entry(query_cache_t::entry_t *entry) {
    delete entry;
}

query_cache_t::ref_t::~ref_t() {
    query_cache->assert_thread();
    guarantee(entry->state != entry_t::state_t::START);

    if (entry->state == entry_t::state_t::DONE) {
        // We do not delete the entry in this context for reasons:
        //  1. If there is an active exception, we aren't allowed to switch coroutines
        //  2. This will block until all auto-drainer locks on the entry have been
        //     removed, including the one in this reference
        // We remove the entry from the cache so no new queries can acquire it
        entry->state = entry_t::state_t::DELETING;

        auto it = query_cache->queries.find(token);
        guarantee(it != query_cache->queries.end());
        coro_t::spawn_sometime(std::bind(&query_cache_t::async_destroy_entry,
                                         it->second.release()));
        query_cache->queries.erase(it);
    }
}

void query_cache_t::ref_t::fill_response(response_t *res) {
    query_cache->assert_thread();
    if (entry->state != entry_t::state_t::START &&
        entry->state != entry_t::state_t::STREAM) {
        // This should only happen if the client recycled a token before
        // getting the response for the last use of the token.
        // In this case, just pretend it's a duplicate token issue
        throw bt_exc_t(Response::CLIENT_ERROR,
            strprintf("ERROR: duplicate token %" PRIi64, token),
            backtrace_registry_t::EMPTY_BACKTRACE);
    }

    try {
        env_t env(query_cache->rdb_ctx,
                  query_cache->return_empty_normal_batches,
                  &combined_interruptor,
                  entry->global_optargs,
                  trace.get_or_null());

        scoped_term_storage_t scoped_term_storage(entry->term_storage, &env);

        if (entry->state == entry_t::state_t::START) {
            run(&env, res);
            entry->root_term.reset();
        }

        if (entry->state == entry_t::state_t::STREAM) {
            serve(&env, res);
        }

        if (trace.has()) {
            res->set_profile(trace->as_datum());
        }
    } catch (const interrupted_exc_t &ex) {
        if (entry->persistent_interruptor.is_pulsed()) {
            if (entry->state != entry_t::state_t::DONE) {
                throw bt_exc_t(Response::RUNTIME_ERROR,
                    "Query terminated by the `rethinkdb.jobs` table.",
                    backtrace_registry_t::EMPTY_BACKTRACE);
            }
            // For compatibility, we return a SUCCESS_SEQUENCE in this case
            res->clear();
            res->set_type(Response::SUCCESS_SEQUENCE);
        } else {
            query_cache->terminate_internal(entry);
            throw;
        }
    } catch (const exc_t &ex) {
        query_cache->terminate_internal(entry);
        throw bt_exc_t(Response::RUNTIME_ERROR, ex.what(),
                       entry->bt_reg.datum_backtrace(ex));
    } catch (const std::exception &ex) {
        query_cache->terminate_internal(entry);
        throw bt_exc_t(Response::RUNTIME_ERROR, ex.what(),
                       backtrace_registry_t::EMPTY_BACKTRACE);
    }
}

void query_cache_t::ref_t::run(env_t *env, response_t *res) {
    // The state will be overwritten if we end up with a stream
    entry->state = entry_t::state_t::DONE;

    scope_env_t scope_env(env, var_scope_t());

    scoped_ptr_t<val_t> val = entry->root_term->eval(&scope_env);
    if (val->get_type().is_convertible(val_t::type_t::DATUM)) {
        res->set_type(Response::SUCCESS_ATOM);
        res->set_data(val->as_datum());
    } else if (counted_t<grouped_data_t> gd =
            val->maybe_as_promiscuous_grouped_data(scope_env.env)) {
        datum_t d = to_datum_for_client_serialization(std::move(*gd), env->limits());
        res->set_type(Response::SUCCESS_ATOM);
        res->set_data(d);
    } else if (val->get_type().is_convertible(val_t::type_t::SEQUENCE)) {
        counted_t<datum_stream_t> seq = val->as_seq(env);
        const datum_t arr = seq->as_array(env);
        if (arr.has()) {
            res->set_type(Response::SUCCESS_ATOM);
            res->set_data(arr);
        } else {
            entry->stream = seq;
            entry->has_sent_batch = false;
            entry->state = entry_t::state_t::STREAM;
        }
    } else {
        rfail_toplevel(base_exc_t::GENERIC,
                       "Query result must be of type "
                       "DATUM, GROUPED_DATA, or STREAM (got %s).",
                       val->get_type().name());
    }
}

void query_cache_t::ref_t::serve(env_t *env, response_t *res) {
    guarantee(entry->stream.has());

    batch_type_t batch_type = entry->has_sent_batch
                                  ? batch_type_t::NORMAL
                                  : batch_type_t::NORMAL_FIRST;
    std::vector<datum_t> ds = entry->stream->next_batch(
            env, batchspec_t::user(batch_type, env));
    entry->has_sent_batch = true;
    res->set_data(std::move(ds));

    // Note that `SUCCESS_SEQUENCE` is possible for feeds if you call `.limit`
    // after the feed.
    if (entry->stream->is_exhausted() || entry->noreply) {
        guarantee(entry->state == entry_t::state_t::STREAM);
        entry->state = entry_t::state_t::DONE;
        res->set_type(Response::SUCCESS_SEQUENCE);
    } else {
        res->set_type(Response::SUCCESS_PARTIAL);
    }

    switch (entry->stream->cfeed_type()) {
    case feed_type_t::not_feed:
        // If we don't have a feed, then a 0-size response means there's no more
        // data.  The reason this `if` statement is only in this branch of the
        // `case` statement is that feeds can sometimes have 0-size responses
        // for other reasons (e.g. in their first batch, or just whenever with a
        // V0_3 protocol).
        if (res->data().size() == 0) res->set_type(Response::SUCCESS_SEQUENCE);
        break;
    case feed_type_t::stream:
        res->add_note(Response::SEQUENCE_FEED);
        break;
    case feed_type_t::point:
        res->add_note(Response::ATOM_FEED);
        break;
    case feed_type_t::orderby_limit:
        res->add_note(Response::ORDER_BY_LIMIT_FEED);
        break;
    case feed_type_t::unioned:
        res->add_note(Response::UNIONED_FEED);
        break;
    default: unreachable();
    }
    entry->stream->set_notes(res);
}

query_cache_t::entry_t::entry_t(const query_params_t &query_params,
                                backtrace_registry_t &&_bt_reg,
                                counted_t<term_storage_t> &&_term_storage,
                                global_optargs_t &&_global_optargs,
                                counted_t<const term_t> _root_term) :
        state(state_t::START),
        job_id(generate_uuid()),
        noreply(query_params.noreply),
        profile(query_params.profile ? profile_bool_t::PROFILE :
                                       profile_bool_t::DONT_PROFILE),
        bt_reg(std::move(_bt_reg)),
        term_storage(std::move(_term_storage)),
        global_optargs(std::move(_global_optargs)),
        start_time(current_microtime()),
        root_term(_root_term),
        has_sent_batch(false) { }

query_cache_t::entry_t::~entry_t() { }

} // namespace ql
