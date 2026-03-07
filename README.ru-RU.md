[![en](https://img.shields.io/badge/lang-en-green.svg)](README.md)

Сервер отчётов
-

**Процесс** для **Apostol CRM**[^crm].

Описание
-

**Сервер отчётов** — фоновый процесс-модуль для фреймворка [Апостол (C++20)](https://github.com/apostoldevel/libapostol). Запускается как отдельный форкнутый процесс и выполняет заранее подготовленные отчёты из базы данных в асинхронном режиме. Отчёт — это PL/pgSQL-процедура, зарегистрированная в очереди отчётов; процесс извлекает её, выполняет как запрос PostgreSQL и обновляет состояние отчёта по завершении.

Основные характеристики:

* Написан на C++20 с использованием асинхронной неблокирующей модели ввода-вывода на базе **epoll** API.
* Подключается к **PostgreSQL** через библиотеку `libpq`, используя роль `apibot` (пул соединений helper).
* Аутентифицируется через OAuth2 `client_credentials` с помощью `BotSession` — повторная аутентификация каждые 24 часа.
* **NOTIFY-driven**: подписывается на PostgreSQL-канал `LISTEN report` для немедленной обработки.
* **Polling-fallback**: каждую минуту проверяет `api.report_ready('enabled')` для обнаружения пропущенных уведомлений.
* **Контроль параллелизма**: параметр `max_in_flight` ограничивает количество одновременно выполняемых отчётов.
* **Очистка зависших отчётов**: отчёты, находящиеся в обработке дольше `report_timeout`, автоматически удаляются для освобождения слотов.
* **Ограниченная очередь NOTIFY**: входящие уведомления ограничены `max_pending` для предотвращения неконтролируемого роста памяти.
* **Поддержка PQcancel**: при отмене отчётов используется `PQcancel` для немедленной остановки выполняемых SQL-запросов.
* Обрабатывает ошибки: для неуспешных отчётов сохраняется сообщение об ошибке; при критических ошибках сервер приостанавливается на 10 секунд.

### Архитектура

Сервер отчётов следует паттерну **ProcessModule**, введённому в apostol.v2:

```
Application
  └── ModuleProcess (generic-оболочка процесса: сигналы, EventLoop, PgPool)
        └── ReportServer (ProcessModule: только бизнес-логика)
```

Жизненный цикл процесса (обработка сигналов, crash recovery, настройка PgPool, таймер heartbeat) управляется generic-оболочкой `ModuleProcess`. `ReportServer` содержит только логику выполнения отчётов.

### Как это работает

```
heartbeat (1 сек)
  └── BotSession::refresh_if_needed()
  └── если аутентифицирован:
        └── process_notify_queue()  — немедленная обработка NOTIFY
        └── если now >= next_check_ (1 мин):
              └── check_reports()   — polling-fallback
                    └── api.authorize(session)
                    └── api.report_ready('enabled') ORDER BY created
                    └── enum_reports():
                          для каждого отчёта:
                            progress && !in_progress → do_start(id)
                              └── api.execute_report_ready(id)
                              └── rpc_* процедуры сами управляют завершением
                              └── ошибка  → do_fail()  → action 'fail' + set_object_label
                            canceled && in_progress → do_abort(id)
                              └── api.execute_object_action(id, 'abort')

NOTIFY "report" → on_notify(payload):
  парсинг JSON → id
  если !in_progress → pending_reports_.push(id)
  → обрабатывается на следующем heartbeat через do_check(id)
    └── api.get_report_ready(id)
    └── если statecode == "progress" → do_start(id)
```

### Формат NOTIFY payload

```json
{"session": "...", "id": "<report-uuid>"}
```

### Машина состояний отчёта (db-platform)

```
created ──enable──► enabled ──execute──► progress ──complete──► completed
                                                   ──fail────► failed
                                                   ──cancel──► canceled ──abort──► aborted
```

Модуль базы данных
-

ReportServer тесно связан с модулем **`report`** платформы [db-platform](https://github.com/apostoldevel/db-platform) (`db/sql/platform/report/`).

Отчёт состоит из четырёх частей:

```
Отчёт
  ├── Tree (дерево)  — иерархия разделов
  ├── Form (форма)   — определение ввода данных (функции rfc_*)
  ├── Routine        — код генерации (функции rpc_*, упорядоченные по sequence)
  └── Ready (готово) — выходной документ (db.report_ready), наследует жизненный цикл Document
```

Ключевые объекты базы данных:

| Объект | Назначение |
|--------|-----------|
| `db.report_ready` | Выходной документ, управляющий выполнением; конечный автомат отправляет `NOTIFY 'report'` при смене состояния |
| `api.report_ready(state)` | Возвращает выходные документы в заданном состоянии (например, `'enabled'`) |
| `api.execute_report_ready(id)` | Запускает подпрограмму(ы) `rpc_*` отчёта и формирует вывод |
| `api.execute_object_action(id, action)` | Переходы состояний: `'complete'`, `'abort'`, `'cancel'`, `'fail'` |
| `api.get_report_ready(id)` | Получает запись report_ready с текущим состоянием |

Конфигурация
-

В конфигурационном файле приложения (`conf/apostol.json`):

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

| Параметр | Тип | По умолчанию | Описание |
|----------|-----|-------------|----------|
| `enable` | bool | `false` | Включить/отключить процесс |
| `heartbeat` | int | `60000` | Интервал проверки отчётов в миллисекундах |
| `max_in_flight` | int | `5` | Максимум одновременных выполнений отчётов |
| `report_timeout` | int | `300000` | Таймаут зависших отчётов в миллисекундах (5 мин). Отчёты в обработке, превысившие этот возраст, удаляются для освобождения слотов параллелизма. |
| `max_pending` | int | `1000` | Максимальный размер очереди NOTIFY. Уведомления, пришедшие при заполненной очереди, отбрасываются (восстанавливаются polling-fallback). |

Также необходимы:
* Строка подключения `postgres.helper` в конфигурации (используется для пула соединений `apibot`)
* Учётные данные OAuth2 `service` в файле `conf/oauth2/default.json`

Установка
-

Следуйте указаниям по сборке и установке [Апостол (C++20)](https://github.com/apostoldevel/libapostol#build-and-installation).

[^crm]: **Apostol CRM** — шаблон-проект построенный на фреймворках [A-POST-OL](https://github.com/apostoldevel/libapostol) (C++20) и [PostgreSQL Framework for Backend Development](https://github.com/apostoldevel/db-platform).
