#ifdef WITH_POSTGRESQL

#include "ReportServer/ReportServer.hpp"

#include "apostol/application.hpp"
#include "apostol/pg_utils.hpp"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>

namespace apostol
{

#ifdef REPORTSERVER_NOTIFY
namespace
{

// How often the mode of the channel is read until it is single, and how often
// a database without the notify module is asked again (a patch may arrive).
constexpr auto mode_every  = std::chrono::seconds(10);
constexpr auto probe_every = std::chrono::seconds(60);

// The shortest lease the path is taken up with: the build is bounded at 8/10
// of it, and the check before the build and the pool's queue take the rest.
constexpr auto lease_floor = std::chrono::seconds(10);

// An outcome written over the row: notify.hold takes the row of the queue
// first, under its token, so the locks go "row, then the object" — the order
// of notify.sweep, which closes the row and then calls the producer on the
// object. "Object, then row" (the ack libapostol appends last) deadlocks with
// it at the end of the lease (T1147). The hold is in the batch of the outcome
// and its ack — one transaction —, or the lock is gone before the outcome.
// A stale token is NT409 here, before anything is written.
std::string held(const db_platform::NotifyRow& row, std::string sql)
{
    if (sql.empty())
        return sql;
    return fmt::format("SELECT notify.hold({}, {});\n", row.id, row.token) + sql;
}

} // namespace
#endif

// --- on_start ----------------------------------------------------------------

void ReportServer::on_start(EventLoop& loop, Application& app)
{
    pool_   = &app.db_pool();
    logger_ = &app.logger();
#ifdef REPORTSERVER_NOTIFY
    loop_   = &loop;
    node_   = app.node_id();
#else
    (void)loop;
#endif

    // Create BotSession for apibot authentication
    bot_ = std::make_unique<BotSession>(*pool_, "ReportServer/2.0", "127.0.0.1");

    // Read OAuth2 credentials from conf/oauth2/default.json -> "service" app
    auto [client_id, client_secret] = app.providers().credentials("service");
    if (!client_id.empty())
        bot_->set_credentials(std::move(client_id), std::move(client_secret));

    // Subscribe to LISTEN "report" for immediate dispatch
    pool_->listen("report", [this](std::string_view /*channel*/, std::string_view payload) {
        on_notify(payload);
    });

    // Read config
    if (auto* cfg = app.module_config("ReportServer")) {
        if (cfg->contains("heartbeat") && (*cfg)["heartbeat"].is_number())
            check_interval_ = milliseconds((*cfg)["heartbeat"].get<int>());
        if (cfg->contains("report_timeout") && (*cfg)["report_timeout"].is_number())
            report_timeout_ = milliseconds((*cfg)["report_timeout"].get<int>());
        if (cfg->contains("max_in_flight") && (*cfg)["max_in_flight"].is_number())
            max_in_flight_ = (*cfg)["max_in_flight"].get<std::size_t>();
        if (cfg->contains("max_pending") && (*cfg)["max_pending"].is_number())
            max_pending_ = (*cfg)["max_pending"].get<std::size_t>();
    }

    logger_->notice("ReportServer started (check_interval={}ms, max_in_flight={}, max_pending={})",
                    check_interval_.count(), max_in_flight_, max_pending_);
}

// --- heartbeat ---------------------------------------------------------------

void ReportServer::heartbeat(std::chrono::system_clock::time_point now)
{
    if (!bot_ || !pool_)
        return;

    bot_->refresh_if_needed();

    if (status_ == Status::stopped) {
        if (bot_->valid())
            status_ = Status::running;
        return;
    }

    // Status::running
    process_notify_queue();

#ifdef REPORTSERVER_NOTIFY
    const auto steady = std::chrono::steady_clock::now();
    if (!notify_possible_ && !probing_ && steady >= next_probe_)
        probe_notify();
    else if (notify_possible_ && !notify_ && !reading_mode_ && steady >= next_mode_)
        read_mode();

    if (notify_)
        start_waiting();
#endif

#ifdef REPORTSERVER_NOTIFY
    // Until the probe has answered the poll does not know whether to ask the
    // mode: unfiltered, right after a start, it would read the reports whose
    // rows another copy holds, and build them beside it.
    const bool may_poll = probed_;
#else
    const bool may_poll = true;
#endif
    if (may_poll && now >= next_check_) {
        check_reports();
        next_check_ = now + check_interval_;
    }

    // Sweep stale reports (stuck in-flight longer than report_timeout_)
    for (auto it = reports_.begin(); it != reports_.end(); ) {
#ifdef REPORTSERVER_NOTIFY
        // Path single: a build is bounded by the statement limits below the
        // lease (body_limits) and ends with its ack; a check that does not
        // answer in time goes back, unchecked.
        if (it->second.row) {
            auto age = std::chrono::duration_cast<milliseconds>(now - it->second.started_at);
            if (!it->second.building && it->second.with_subscriber && age > report_timeout_) {
                const auto id = it->first;
                ++it;
                logger_->warn("ReportServer: report {} not checked in {}ms — given back", id, age.count());
                give_back(id, "unsent: the report could not be checked in time");
            } else {
                ++it;
            }
            continue;
        }
#endif
        auto age = std::chrono::duration_cast<milliseconds>(now - it->second.started_at);
        if (age > report_timeout_) {
            logger_->warn("ReportServer: report {} timed out after {}ms, removing",
                         it->first, age.count());
            it = reports_.erase(it);
        } else {
            ++it;
        }
    }
}

// --- on_stopping / busy / on_stop -------------------------------------------
//
// Finish what is started (the owner's В4): nothing new is taken — the
// heartbeat is not called from here on —, and on the path single a report
// not being built yet goes back to the queue for another copy. A build is one
// statement with its ack, the pool's to wait for.

void ReportServer::on_stopping()
{
    stopping_ = true;

#ifdef REPORTSERVER_NOTIFY
    if (!notify_)
        return;

    // Not chosen as an addressee any more, nothing new taken.
    notify_->drain();

    while (!waiting_.empty()) {
        notify_->refuse(waiting_.front().row, "unsent: stopping, not started");
        waiting_.pop_front();
    }

    std::vector<std::string> unchecked;
    for (const auto& [id, report] : reports_) {
        if (report.row && report.with_subscriber)
            unchecked.push_back(id);
    }
    for (const auto& id : unchecked)
        give_back(id, "unsent: stopping, not built yet");
#endif
}

// Rows with the subscriber — taken, no build or outcome sent — after
// on_stopping gave back what it could: none, unless a check answered late.
bool ReportServer::busy() const
{
#ifdef REPORTSERVER_NOTIFY
    return notify_ && notify_->busy();
#else
    return false;
#endif
}

void ReportServer::on_stop()
{
    if (pool_)
        pool_->unlisten("report");

#ifdef REPORTSERVER_NOTIFY
    // The rows still with the subscriber — taken, no build sent (a check that
    // answered after on_stopping) — go back as `unsent:`, and it is
    // deregistered, on the listener; the default reason (`stopping:`) would
    // close them, the report failed unbuilt. A build sent with its ack is not
    // among them: committed, it is confirmed; cut off by the stop, it is
    // rolled back and its row returns by the lease.
    if (notify_)
        notify_->stop("unsent: stopping, not built yet");
    notify_.reset();
#endif

    if (bot_)
        bot_->sign_out();
    bot_.reset();
}

// --- on_notify ---------------------------------------------------------------
//
// Called from PgPool listener when a NOTIFY arrives on the "report" channel.
// Payload format: {"session": "...", "id": "<report-uuid>"}
//

void ReportServer::on_notify(std::string_view payload)
{
    try {
        auto j = nlohmann::json::parse(payload);
        if (j.contains("id") && j["id"].is_string()) {
            auto id = j["id"].get<std::string>();

            // The notification names the session it came from —
            // pg_notify('report', json_build_object('session', current_session(), …)).
            // Taking it saves guessing which scope the report lives in, and it is
            // the requester's own session, so it authorizes into the right one.
            auto session = j.contains("session") && j["session"].is_string()
                               ? j["session"].get<std::string>()
                               : std::string();

            if (!id.empty() && !in_progress(id) && pending_reports_.size() < max_pending_)
                pending_reports_.push_back(Pending{std::move(id), std::move(session)});
        }
    } catch (const nlohmann::json::exception& e) {
        if (logger_)
            logger_->error("ReportServer: bad NOTIFY payload: {}", e.what());
    }
}

// --- process_notify_queue ----------------------------------------------------

void ReportServer::process_notify_queue()
{
    if (pending_reports_.empty())
        return;

    // Move out to avoid re-entrancy issues
    auto pending = std::move(pending_reports_);
    pending_reports_.clear();

    for (std::size_t i = 0; i < pending.size(); ++i) {
        if (reports_.size() >= max_in_flight_) {
            // Put unprocessed IDs back for the next heartbeat cycle
            for (; i < pending.size(); ++i)
                pending_reports_.push_back(std::move(pending[i]));
            break;
        }
        if (!in_progress(pending[i].id))
            do_check(pending[i].session, pending[i].id);
    }
}

// --- check_reports -----------------------------------------------------------
//
// Polling fallback (every check_interval_):
//   1. api.authorize(session)
//   2. api.report_ready('enabled') ORDER BY created
//

void ReportServer::check_reports()
{
    if (!bot_->valid())
        return;

    // One pass per scope: api.report_ready answers within the authorized session's
    // scope, so a single pass under the first session leaves every other scope's
    // reports unseen. v1 looped over api.get_sessions; the port kept one.
    // The poll goes around the claim of the path single: a report whose row
    // another copy holds is `progress` while it is built, and a poll reading
    // it then would build it twice. With the notify module the mode is asked
    // in the same statement — a channel switched to single makes the poll
    // empty at once, not after the next reading of the mode.
#ifdef REPORTSERVER_NOTIFY
    const char* filter = notify_possible_
        ? " WHERE (SELECT mode FROM notify.channel_policy('report')) IS DISTINCT FROM 'single'"
        : "";
#else
    const char* filter = "";
#endif

    for (const auto& session : bot_->sessions()) {
        auto sql = fmt::format(
            "SELECT * FROM api.authorize({});\n"
            "SELECT * FROM api.report_ready('enabled'){} ORDER BY created",
            pq_quote_literal(session), filter);

        pool_->execute(sql,
            [this, session](std::vector<PgResult> results) {
                enum_reports(session, std::move(results));
            },
            [this](std::string_view error) {
                on_fatal(std::string(error));
            },
            /*quiet=*/true);
    }
}

// --- enum_reports ------------------------------------------------------------
//
// Mirrors v1 CReportServer::EnumReportReady():
//   For each report in results:
//     - state == "progress" && !in_progress -> do_start
//     - state == "canceled" && in_progress  -> do_abort
//

void ReportServer::enum_reports(const std::string& session, std::vector<PgResult> results)
{
    // results[0] = authorize, results[1] = report_ready list
    if (results.size() < 2 || !results[1].ok())
        return;

    auto& res = results[1];
    int rows = res.rows();

    int col_id        = res.column_index("id");
    int col_statecode = res.column_index("statecode");

    if (col_id < 0 || col_statecode < 0)
        return;

    for (int r = 0; r < rows; ++r) {
        std::string id    = res.value(r, col_id)        ? res.value(r, col_id)        : "";
        std::string state = res.value(r, col_statecode) ? res.value(r, col_statecode) : "";

        if (id.empty())
            continue;

#ifdef REPORTSERVER_NOTIFY
        if (single(id))
            continue;   // the path single's — its row decides, not the poll
#endif

        if (in_progress(id)) {
            if (state == "canceled")
                do_abort(id);
        } else {
            if (state == "progress" && reports_.size() < max_in_flight_)
                do_start(session, id);
        }
    }
}

// --- do_check ----------------------------------------------------------------
//
// Verify a single report (from NOTIFY) before starting:
//   api.authorize(session) + api.get_report_ready(id)
//   If statecode == "progress" -> do_start
//

void ReportServer::do_check(const std::string& session, const std::string& id)
{
    if (!bot_ || !bot_->valid())
        return;

    // The notification's own session when there is one, ours otherwise — a report
    // found by polling was enumerated under one of ours.
    const auto& use = session.empty() ? bot_->session() : session;

    auto sql = fmt::format(
        "SELECT * FROM api.authorize({});\n"
        "SELECT id, statecode FROM api.get_report_ready({}::uuid)",
        pq_quote_literal(use),
        pq_quote_literal(id));

    // quiet: the statement carries a session code. PgPool prints statement
    // text at debug into postgres.log — inside the container, readable
    // by any process there.
    pool_->execute(sql,
        [this, id, use](std::vector<PgResult> results) {
            if (results.size() < 2 || !results[1].ok())
                return;

            auto& res = results[1];
            if (res.rows() == 0)
                return;

            int col_statecode = res.column_index("statecode");
            if (col_statecode < 0)
                return;

            std::string state = res.value(0, col_statecode)
                                    ? res.value(0, col_statecode) : "";
            if (state == "progress" && !in_progress(id))
                do_start(use, id);
        },
        [this](std::string_view error) {
            on_fatal(std::string(error));
        },
        /*quiet=*/true);
}

// --- do_start ----------------------------------------------------------------
//
// Execute the report:
//   api.authorize(session) + api.execute_report_ready(id)
//

void ReportServer::do_start(const std::string& session, const std::string& id)
{
    auto& report      = reports_[id];
    report.id         = id;
    report.session    = session;
    report.started_at = std::chrono::system_clock::now();

    logger_->debug("ReportServer: starting report {}", id);

    if (!bot_ || !bot_->valid()) {
        delete_report(id);
        return;
    }

    auto sql = fmt::format(
        "SELECT * FROM api.authorize({});\n"
        "SELECT * FROM api.execute_report_ready({}::uuid)",
        pq_quote_literal(session),
        pq_quote_literal(id));

    // quiet: the statement carries a session code. PgPool prints statement
    // text at debug into postgres.log — inside the container, readable
    // by any process there.
    auto qid = pool_->execute(sql,
        [this, id](std::vector<PgResult> /*results*/) {
            // rpc_* routines handle state transitions themselves
            // (sync: DoAction 'complete', async: callback completes later)
            delete_report(id);
        },
        [this, id](std::string_view error) {
            if (in_progress(id))
                do_fail(id, std::string(error));
            else
                delete_report(id);
        },
        /*quiet=*/true);

    // Store query handle for cancel support
    auto it = reports_.find(id);
    if (it != reports_.end())
        it->second.query_id = qid;
}

// --- do_complete -------------------------------------------------------------

void ReportServer::do_complete(const std::string& id)
{
    logger_->debug("ReportServer: report {} complete", id);

    execute_action(id, "complete",
        [this, id](std::vector<PgResult> /*results*/) {
            delete_report(id);
        });
}

// --- do_abort ----------------------------------------------------------------

void ReportServer::do_abort(const std::string& id)
{
    logger_->notice("ReportServer: aborting report {}", id);

    // Cancel running SQL body if any (PQcancel → PostgreSQL)
    auto it = reports_.find(id);
    if (it != reports_.end() && it->second.query_id != 0)
        pool_->cancel(it->second.query_id);

    // Remove from tracking immediately — canceled query results will be discarded
    delete_report(id);

    execute_action(id, "abort",
        [](std::vector<PgResult> /*results*/) {
            // report already removed from reports_
        });
}

// --- do_fail -----------------------------------------------------------------
//
// Mark report as failed + store error as object label.
//

void ReportServer::do_fail(const std::string& id, const std::string& error)
{
    logger_->error("ReportServer: report {} failed: {}", id, error);

    if (!bot_ || !bot_->valid()) {
        delete_report(id);
        return;
    }

    auto sql = fmt::format(
        "SELECT * FROM api.authorize({});\n"
        "SELECT * FROM api.execute_object_action({}::uuid, {});\n"
        "SELECT * FROM api.set_object_label({}::uuid, {})",
        pq_quote_literal(report_session(id)),
        pq_quote_literal(id), pq_quote_literal("fail"),
        pq_quote_literal(id), pq_quote_literal(error));

    // quiet: the statement carries a session code. PgPool prints statement
    // text at debug into postgres.log — inside the container, readable
    // by any process there.
    pool_->execute(sql,
        [this, id](std::vector<PgResult> /*results*/) {
            delete_report(id);
        },
        [this, id](std::string_view err) {
            logger_->error("ReportServer: do_fail SQL error for {}: {}", id, err);
            delete_report(id);
        },
        /*quiet=*/true);
}

// --- execute_action ----------------------------------------------------------

void ReportServer::execute_action(const std::string& id, std::string_view action,
                                  PgQuery::ResultHandler on_result)
{
    bot_->execute_action(report_session(id), id, action, std::move(on_result),
        [this, id, act = std::string(action)](std::string_view error) {
            logger_->error("ReportServer: action '{}' failed for {}: {}", act, id, error);
            delete_report(id);
            on_fatal(std::string(error));
        });
}

// --- delete_report / in_progress ---------------------------------------------

std::string ReportServer::report_session(const std::string& id) const
{
    auto it = reports_.find(id);
    if (it != reports_.end() && !it->second.session.empty())
        return it->second.session;
    return bot_ ? bot_->session() : std::string();
}

void ReportServer::delete_report(const std::string& id)
{
    reports_.erase(id);
}

bool ReportServer::in_progress(const std::string& id) const
{
    return reports_.count(id) > 0;
}

#ifdef REPORTSERVER_NOTIFY

// --- path single: the mode ---------------------------------------------------
//
// A database without the notify module (an older db-platform) has the path
// legacy only; it is asked again now and then. With the module the mode of
// `report` is read until it is single; from then on the subscriber stays for
// the life of the process — a channel returned to legacy still has rows to
// bring to their end, and notify.send alone decides which path a new report
// takes (the LISTEN on "report" serves the other one meanwhile).

void ReportServer::probe_notify()
{
    probing_ = true;

    pool_->execute(
        "SELECT to_regprocedure('notify.channel_policy(text)') IS NOT NULL"
        " AND to_regprocedure('notify.take(bigint,integer)') IS NOT NULL",
        [this](std::vector<PgResult> results) {
            probing_ = false;
            next_probe_ = std::chrono::steady_clock::now() + probe_every;
            if (results.empty() || !results[0].ok())
                return;   // not answered: asked again, the poll waits

            const bool yes = results[0].rows() > 0 &&
                             results[0].value(0, 0) != nullptr && results[0].value(0, 0)[0] == 't';
            if (yes && !notify_possible_)
                logger_->notice("ReportServer: the database has the notify module — the channel report is "
                                "taken from notify.queue when it is single");
            notify_possible_ = yes;
            probed_ = true;
        },
        [this](std::string_view error) {
            probing_ = false;
            next_probe_ = std::chrono::steady_clock::now() + probe_every;
            logger_->error("ReportServer: the notify module is not probed: {}", error);
        });
}

void ReportServer::read_mode()
{
    reading_mode_ = true;
    next_mode_ = std::chrono::steady_clock::now() + mode_every;

    pool_->execute(
        "SELECT mode, (extract(epoch FROM lease) * 1000)::bigint FROM notify.channel_policy('report')",
        [this](std::vector<PgResult> results) {
            reading_mode_ = false;

            if (notify_ || stopping_)
                return;

            // No row: no channel report — nothing but the path legacy.
            if (results.empty() || !results[0].ok() || results[0].rows() == 0 || results[0].columns() < 2)
                return;

            const char* mode = results[0].value(0, 0);
            if (mode == nullptr || std::string_view(mode) != "single")
                return;

            const char* lease = results[0].value(0, 1);
            const long lease_ms = lease != nullptr ? std::strtol(lease, nullptr, 10) : 0;

            if (milliseconds(lease_ms) < lease_floor) {
                if (lease_ms != refused_lease_)
                    logger_->error("ReportServer: the channel report is single, its path is not taken up: lease "
                                   "{} ms is shorter than {} s — no room for a build", lease_ms, lease_floor.count());
                refused_lease_ = lease_ms;
                return;
            }

            lease_ = milliseconds(lease_ms);
            start_notify();
        },
        [this](std::string_view error) {
            reading_mode_ = false;
            logger_->error("ReportServer: the mode of the channel report is not read: {}", error);
        });
}

void ReportServer::start_notify()
{
    db_platform::NotifySubscriber::Options options;
    options.channel     = "report";
    options.node        = node_;
    options.application = "ReportServer";
    options.role        = "report-server";

    notify_ = std::make_unique<db_platform::NotifySubscriber>(*pool_, *loop_, *logger_, std::move(options),
        [this](const db_platform::NotifyRow& row) { on_row(row); });

    logger_->notice("ReportServer: the channel report is single — reports are taken from notify.queue "
                    "(lease {} ms, a build is bounded at 8/10 of it)", lease_.count());
    notify_->start();
}

// --- path single: a row ------------------------------------------------------

std::optional<ReportServer::Pending> ReportServer::parse_payload(std::string_view payload)
{
    const auto j = nlohmann::json::parse(payload, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object())
        return std::nullopt;

    Pending p;
    if (auto it = j.find("id"); it != j.end() && it->is_string())
        p.id = it->get<std::string>();
    if (auto it = j.find("session"); it != j.end() && it->is_string())
        p.session = it->get<std::string>();

    if (p.id.empty())
        return std::nullopt;
    return p;
}

void ReportServer::on_row(const db_platform::NotifyRow& row)
{
    if (stopping_) {
        notify_->refuse(row, "unsent: stopping, taken after the stop began");
        return;
    }

    // Not logged: the payload may carry a session code (D.DEV.02).
    const auto pending = parse_payload(row.payload);
    if (!pending) {
        logger_->error("ReportServer: row {} of report names no report — confirmed, nothing to build", row.id);
        notify_->complete(row, {});
        return;
    }

    const std::string& id = pending->id;

    auto it = reports_.find(id);
    if (it != reports_.end()) {
        if (!it->second.row) {
            // Built on the path legacy here (a switch in progress): that one ends it.
            logger_->notice("ReportServer: report {} came by notify.queue while built on the path legacy — "
                            "confirmed, built once", id);
            notify_->complete(row, {});
            return;
        }
        // Handed out again while still here: the earlier delivery's ack is
        // refused under its old token and rolls its build back — this one is
        // the run, from the start.
        logger_->warn("ReportServer: report {} delivered again (attempt {}) while still here — "
                      "the earlier run will be rolled back", id, row.attempts);
        reports_.erase(it);
    }

    for (auto& w : waiting_) {
        if (w.pending.id == id) {
            w.row = row;
            w.pending = *pending;
            return;
        }
    }

    waiting_.push_back(Waiting{row, *pending, std::chrono::steady_clock::now()});
    start_waiting();
}

// Rows wait here while max_in_flight reports are out. One that waited half
// its lease is given back, unsent — another copy may have room; and a row is
// not started without a bot session to act with.
void ReportServer::start_waiting()
{
    if (!notify_ || stopping_)
        return;

    const auto now = std::chrono::steady_clock::now();
    for (auto it = waiting_.begin(); it != waiting_.end(); ) {
        if (lease_.count() > 0 && now - it->taken > lease_ / 2) {
            logger_->warn("ReportServer: report {} waited here past half the lease — given back", it->pending.id);
            notify_->refuse(it->row, "unsent: waited for a free slot past half the lease");
            it = waiting_.erase(it);
        } else {
            ++it;
        }
    }

    if (!bot_ || !bot_->valid())
        return;

    while (!waiting_.empty() && reports_.size() < max_in_flight_) {
        auto w = std::move(waiting_.front());
        waiting_.pop_front();
        check_single(w.row, w.pending);
    }
}

// The state decides: `progress` is built; `canceled` (canceled before the
// build began) is aborted; anything else has nothing to build.
void ReportServer::check_single(const db_platform::NotifyRow& row, const Pending& pending)
{
    const std::string id = pending.id;

    // The row's own session when it has one, ours otherwise.
    auto& report           = reports_[id];
    report                 = Report{};
    report.id              = id;
    report.session         = pending.session.empty() ? bot_->session() : pending.session;
    report.started_at      = std::chrono::system_clock::now();
    report.row             = row;
    report.with_subscriber = true;

    auto sql = fmt::format(
        "SELECT * FROM api.authorize({});\n"
        "SELECT id, statecode FROM api.get_report_ready({}::uuid)",
        pq_quote_literal(report.session),
        pq_quote_literal(id));

    // quiet: the statement carries a session code (see do_check).
    pool_->execute(sql,
        [this, id, token = row.token](std::vector<PgResult> results) {
            auto it = reports_.find(id);
            if (it == reports_.end() || !it->second.row || it->second.row->token != token ||
                !it->second.with_subscriber || it->second.building)
                return;   // given back, or delivered again meanwhile

            const auto row = *it->second.row;

            const bool failed = results.size() < 2 || std::any_of(results.begin(), results.end(),
                [](const PgResult& r) { return !r.ok(); });
            if (failed) {
                give_back(id, "unsent: the report could not be checked");
                return;
            }

            const auto& res = results[1];
            const int col_statecode = res.column_index("statecode");
            const std::string state = (res.rows() > 0 && col_statecode >= 0 && res.value(0, col_statecode))
                                          ? res.value(0, col_statecode) : "";

            if (state == "progress") {
                build_single(id);
                return;
            }

            if (state == "canceled") {
                logger_->notice("ReportServer: report {} canceled before its build — aborted", id);
                finish(id, row, action_sql(id, "abort"));
                return;
            }

            logger_->notice("ReportServer: report {} is '{}' — confirmed, nothing to build", id,
                            state.empty() ? "not visible" : state);
            finish(id, row, {});
        },
        [this, id, token = row.token](std::string_view error) {
            logger_->error("ReportServer: report {} not checked: {}", id, error);
            auto it = reports_.find(id);
            if (it != reports_.end() && it->second.row && it->second.row->token == token &&
                it->second.with_subscriber && !it->second.building)
                give_back(id, "unsent: the report could not be checked");
        },
        /*quiet=*/true);
}

// --- path single: the build --------------------------------------------------
//
// One transaction:
//   1. notify.hold — the row first, under its token (see held)
//   2. the limits below the lease
//   3. api.authorize(session)
//   4. api.execute_report_ready(id) — the routines complete the report themselves
//   5. notify.ack — appended by NotifySubscriber
// A stale token (NT409) on the hold or the ack rolls the build back with it.
// The row stays held while the report is built: notify.sweep passes it by
// (SKIP LOCKED) and gives it to nobody else while this sender is alive.

void ReportServer::build_single(const std::string& id)
{
    auto it = reports_.find(id);
    if (it == reports_.end() || !it->second.row || !notify_)
        return;

    auto& report = it->second;
    report.building        = true;
    report.with_subscriber = false;
    const auto row = *report.row;

    logger_->debug("ReportServer: building report {}", id);

    auto sql = body_limits() + fmt::format(
        "SELECT * FROM api.authorize({});\n"
        "SELECT * FROM api.execute_report_ready({}::uuid)",
        pq_quote_literal(report.session),
        pq_quote_literal(id));

    // quiet: the statement carries a session code (see do_check).
    notify_->complete(row, held(row, std::move(sql)),
        [this, id, token = row.token](std::vector<PgResult> /*results*/) {
            forget(id, token);
        },
        [this, id, row](std::string_view error, bool stale) {
            if (stale) {
                logger_->warn("ReportServer: report {}: the row is not this delivery's any more — the build "
                              "is rolled back", id);
                forget(id, row.token);
                return;
            }
            after_build_error(id, row, std::string(error));
        },
        /*quiet=*/true);
}

// The build failed — or its answer was lost with the connection, the outcome
// unknown. The state decides, each outcome with the ack: `canceled` (the
// routines' `complete` is refused there) → `abort`; `progress` → `fail` and the
// label; anything else (built after all, or moved on) — confirmed.
void ReportServer::after_build_error(const std::string& id, const db_platform::NotifyRow& row,
                                     const std::string& error)
{
    auto it = reports_.find(id);
    if (it == reports_.end() || !it->second.row || it->second.row->token != row.token)
        return;

    logger_->error("ReportServer: report {} failed: {}", id, error);

    // Inside a $report$ body: a label carrying the tag would end it early.
    std::string why(error);
    std::erase(why, '$');

    const auto quoted = pq_quote_literal(id);
    auto sql = fmt::format(
        "SELECT * FROM api.authorize({});\n"
        "DO $report$ DECLARE s text; BEGIN"
        " SELECT statecode INTO s FROM api.get_report_ready({}::uuid);"
        " IF s = 'canceled' THEN PERFORM api.execute_object_action({}::uuid, 'abort');"
        " ELSIF s = 'progress' THEN PERFORM api.execute_object_action({}::uuid, 'fail');"
        " PERFORM api.set_object_label({}::uuid, {});"
        " END IF; END $report$",
        pq_quote_literal(it->second.session), quoted, quoted, quoted, quoted, pq_quote_literal(why));

    finish_direct(id, row, std::move(sql));
}

// --- path single: the outcome ------------------------------------------------

std::string ReportServer::action_sql(const std::string& id, std::string_view action) const
{
    return fmt::format("SELECT * FROM api.authorize({});\n"
                       "SELECT * FROM api.execute_object_action({}::uuid, {})",
                       pq_quote_literal(report_session(id)), pq_quote_literal(id),
                       pq_quote_literal(std::string(action)));
}

// An outcome with the ack, for a row still with the subscriber.
void ReportServer::finish(const std::string& id, const db_platform::NotifyRow& row, std::string sql)
{
    auto it = reports_.find(id);
    if (it == reports_.end() || !it->second.row || it->second.row->token != row.token)
        return;

    if (!notify_ || !it->second.with_subscriber) {
        finish_direct(id, row, std::move(sql));
        return;
    }

    it->second.with_subscriber = false;

    notify_->complete(row, held(row, sql),
        [this, id, token = row.token](std::vector<PgResult>) { forget(id, token); },
        [this, id, row, sql](std::string_view error, bool stale) {
            if (stale) {
                forget(id, row.token);
                return;
            }
            logger_->warn("ReportServer: report {}: outcome and confirmation not written ({}) — sent again", id, error);
            finish_direct(id, row, sql);
        },
        /*quiet=*/true);
}

// The same statement sent by the module itself — the row is not the
// subscriber's any more —, on a pool that resends after a lost connection:
// committed the first time, it is refused by the hold (NT409) and changes
// nothing.
void ReportServer::finish_direct(const std::string& id, const db_platform::NotifyRow& row, std::string sql)
{
    pool_->execute(db_platform::NotifySubscriber::ack_sql(row, held(row, std::move(sql))),
        [this, id, token = row.token](std::vector<PgResult> results) {
            const auto bad = std::find_if(results.begin(), results.end(),
                [](const PgResult& r) { return !r.ok(); });
            if (bad != results.end() && std::string_view(bad->sqlstate()) != "NT409")
                logger_->error("ReportServer: report {}: outcome not written, its row returns by the lease: {}",
                               id, bad->error_message());
            forget(id, token);
        },
        {}, /*quiet=*/true, PgRetry::if_lost);
}

// Read but not built: back to the queue for another copy.
void ReportServer::give_back(const std::string& id, std::string_view reason)
{
    auto it = reports_.find(id);
    if (it == reports_.end() || !it->second.row)
        return;

    const auto row = *it->second.row;
    const bool with_subscriber = it->second.with_subscriber;
    reports_.erase(it);
    if (notify_ && with_subscriber)
        notify_->refuse(row, reason);
}

void ReportServer::forget(const std::string& id, std::int64_t token)
{
    auto it = reports_.find(id);
    if (it != reports_.end() && it->second.row && it->second.row->token == token)
        reports_.erase(it);
}

bool ReportServer::single(const std::string& id) const
{
    auto it = reports_.find(id);
    return it != reports_.end() && it->second.row.has_value();
}

// Each statement below 8/10 of the lease — the error is the build's own
// ("canceling statement due to statement timeout"), the report fails. The
// whole transaction below 9/10 where the server has transaction_timeout (17
// and later), at the cost of the connection — the server ends the session.
// The same limits as a job body (TaskScheduler, T1104). Never '0': that is
// "no limit".
std::string ReportServer::body_limits() const
{
    if (lease_ <= milliseconds(0))
        return {};

    return fmt::format(
        "SELECT set_config('statement_timeout', '{}', true);\n"
        "SELECT set_config('transaction_timeout', '{}', true) FROM pg_settings WHERE name = 'transaction_timeout';\n",
        std::max<std::int64_t>(1, lease_.count() * 8 / 10), std::max<std::int64_t>(1, lease_.count() * 9 / 10));
}

#endif // REPORTSERVER_NOTIFY

// --- on_fatal ----------------------------------------------------------------
//
// Catastrophic error -- pause for 10 seconds before retrying.
//

void ReportServer::on_fatal(const std::string& error)
{
    status_ = Status::stopped;
    next_check_ = std::chrono::system_clock::now() + std::chrono::seconds(10);
    logger_->error("ReportServer: fatal error, pausing 10s: {}", error);
}

} // namespace apostol

#endif // WITH_POSTGRESQL
