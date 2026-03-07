[![ru](https://img.shields.io/badge/lang-ru-green.svg)](README.ru-RU.md)

Report Server
-

**Process** for **Apostol CRM**[^crm].

Description
-

**Report Server** is a background process module for the [Apostol (C++20)](https://github.com/apostoldevel/libapostol) framework. It runs as an independent forked process and executes pre-defined database reports asynchronously. A report is a PL/pgSQL routine registered in the report queue; the process picks it up, runs it as a PostgreSQL query, and updates the report's state when done.

Key characteristics:

* Written in C++20 using an asynchronous, non-blocking I/O model based on the **epoll** API.
* Connects to **PostgreSQL** via the `libpq` library using the `apibot` database role (helper connection pool).
* Authenticates via OAuth2 `client_credentials` grant using `BotSession` — re-authenticates every 24 hours.
* **NOTIFY-driven**: subscribes to PostgreSQL `LISTEN report` channel for immediate dispatch.
* **Polling fallback**: checks `api.report_ready('enabled')` every minute to catch missed notifications.
* **Concurrency control**: `max_in_flight` parameter bounds the number of reports being executed simultaneously.
* **Stale report cleanup**: reports stuck in-flight longer than `report_timeout` are automatically removed.
* **Bounded NOTIFY queue**: incoming notifications are capped at `max_pending` to prevent unbounded memory growth.
* **PQcancel support**: canceled reports trigger `PQcancel` to stop running SQL queries immediately.
* Handles error recovery: failed reports are marked with the error message; the server pauses for 10 seconds on fatal errors.

### Architecture

Report Server follows the **ProcessModule** pattern introduced in apostol.v2:

```
Application
  └── ModuleProcess (generic process shell: signals, EventLoop, PgPool)
        └── ReportServer (ProcessModule: business logic only)
```

The process lifecycle (signal handling, crash recovery, PgPool setup, heartbeat timer) is managed by the generic `ModuleProcess` shell. `ReportServer` only contains the report execution logic.

### How it works

```
heartbeat (1s)
  └── BotSession::refresh_if_needed()
  └── if authenticated:
        └── process_notify_queue()  — immediate NOTIFY dispatch
        └── if now >= next_check_ (1 min):
              └── check_reports()   — polling fallback
                    └── api.authorize(session)
                    └── api.report_ready('enabled') ORDER BY created
                    └── enum_reports():
                          for each report:
                            progress && !in_progress → do_start(id)
                              └── api.execute_report_ready(id)
                              └── rpc_* routines handle completion internally
                              └── error   → do_fail()  → action 'fail' + set_object_label
                            canceled && in_progress → do_abort(id)
                              └── api.execute_object_action(id, 'abort')

NOTIFY "report" → on_notify(payload):
  parse JSON → id
  if !in_progress → pending_reports_.push(id)
  → processed on next heartbeat via do_check(id)
    └── api.get_report_ready(id)
    └── if statecode == "progress" → do_start(id)
```

### NOTIFY payload format

```json
{"session": "...", "id": "<report-uuid>"}
```

### Report state machine (db-platform)

```
created ──enable──► enabled ──execute──► progress ──complete──► completed
                                                   ──fail────► failed
                                                   ──cancel──► canceled ──abort──► aborted
```

Database module
-

ReportServer is tightly coupled to the **`report`** module of [db-platform](https://github.com/apostoldevel/db-platform) (`db/sql/platform/report/`).

A report consists of four parts:

```
Report
  ├── Tree      — section hierarchy
  ├── Form      — user input definition (rfc_* function)
  ├── Routine   — generation code (rpc_* functions, ordered by sequence)
  └── Ready     — output document (db.report_ready), inherits Document lifecycle
```

Key database objects:

| Object | Purpose |
|--------|---------|
| `db.report_ready` | Output document that drives execution; state machine triggers `NOTIFY 'report'` on state change |
| `api.report_ready(state)` | Returns output documents in the given state (e.g. `'enabled'`) |
| `api.execute_report_ready(id)` | Runs the report's `rpc_*` routine(s) and produces output |
| `api.execute_object_action(id, action)` | State transitions: `'complete'`, `'abort'`, `'cancel'`, `'fail'` |
| `api.get_report_ready(id)` | Fetches report_ready record with current state |

Configuration
-

In the application config (`conf/apostol.json`):

```json
{
  "module": {
    "ReportServer": {
      "enable": true,
      "heartbeat": 60000,
      "max_in_flight": 5,
      "report_timeout": 300000,
      "max_pending": 1000
    }
  }
}
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `enable` | bool | `false` | Enable/disable the process |
| `heartbeat` | int | `60000` | Report check interval in milliseconds |
| `max_in_flight` | int | `5` | Maximum concurrent report executions |
| `report_timeout` | int | `300000` | Stale report timeout in milliseconds (5 min). In-flight reports exceeding this age are removed to free concurrency slots. |
| `max_pending` | int | `1000` | Maximum NOTIFY queue size. Notifications arriving when the queue is full are dropped (recovered by polling fallback). |

The process also requires:
* `postgres.helper` connection string in the config (used for the `apibot` connection pool)
* OAuth2 `service` credentials in `conf/oauth2/default.json`

Installation
-

Follow the build and installation instructions for [Apostol (C++20)](https://github.com/apostoldevel/libapostol#build-and-installation).

[^crm]: **Apostol CRM** — a template project built on the [A-POST-OL](https://github.com/apostoldevel/libapostol) (C++20) and [PostgreSQL Framework for Backend Development](https://github.com/apostoldevel/db-platform) frameworks.
