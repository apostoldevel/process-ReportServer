[![en](https://img.shields.io/badge/lang-en-green.svg)](README.md)

Сервер отчётов
-
**ReportServer** — процесс для [Apostol](https://github.com/apostoldevel/apostol) + [db-platform](https://github.com/apostoldevel/db-platform) — **Apostol CRM**[^crm].

Описание
-
**ReportServer** выполняет заранее подготовленные отчёты из базы данных в асинхронном режиме. Отчёт — это PL/pgSQL-процедура, зарегистрированная в очереди отчётов; процесс извлекает её, выполняет как запрос PostgreSQL и обновляет состояние отчёта по завершении.

Процесс работает независимо внутри мастер-процесса Апостол, используя общий цикл событий на основе `epoll` — без потоков, без блокирующего ввода-вывода.

Архитектура
-
`CReportProcess` — обёртка на уровне ОС-процесса. Она содержит `CReportServer`, который наследует `CQueueCollection` и `CApostolModule` и реализует логику выполнения отчётов.

Принцип работы
-
1. Авторизуется через OAuth2 `client_credentials` как `apibot`; повторная авторизация каждые 24 часа.
2. Подписывается на PostgreSQL-канал уведомлений `report` через `LISTEN report`.
3. Каждую минуту опрашивает `api.report_ready('enabled')` как резервный механизм.
4. Для каждого отчёта в состоянии `progress` вызывает `api.execute_report_ready(id)`.
5. По завершении — `api.execute_object_action(id, 'complete')`.
6. При отмене — отменяет активный PQ-запрос, затем `api.execute_object_action(id, 'abort')`.
7. При ошибке — `api.execute_object_action(id, 'fail')`.

Формат полезной нагрузки уведомления в канале `report`:

```json
{"session": "...", "id": "<report-uuid>"}
```

Жизненный цикл отчёта
-

| Состояние | Действие |
|-----------|---------|
| `progress` | Выполнить `api.execute_report_ready(id)` |
| _(запрос завершён)_ | `execute_object_action('complete')` |
| `canceled` _(во время выполнения)_ | Отменить PQ-запрос → `execute_object_action('abort')` |
| _(ошибка запроса)_ | `execute_object_action('fail')` |

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

Настройка
-
```ini
[module/ReportServer]
enable=true
```

> **Важно:** Секция конфигурации — `module/ReportServer`, а не `process/ReportServer`. Это связано с тем, что встроенный модуль `CReportServer` читает флаг `enable` из пространства имён `module/`.

Установка
-
Следуйте указаниям по сборке и установке [Апостол](https://github.com/apostoldevel/apostol#%D1%81%D0%B1%D0%BE%D1%80%D0%BA%D0%B0-%D0%B8-%D1%83%D1%81%D1%82%D0%B0%D0%BD%D0%BE%D0%B2%D0%BA%D0%B0).

[^crm]: **Apostol CRM** — абстрактный термин, а не самостоятельный продукт. Он обозначает любой проект, в котором совместно используются фреймворк [Apostol](https://github.com/apostoldevel/apostol) (C++) и [db-platform](https://github.com/apostoldevel/db-platform) через специально разработанные модули и процессы. Каждый фреймворк можно использовать независимо; вместе они образуют полноценную backend-платформу.
