# Warax Launcher 2.0 (C++ / WebView2)

Нативный лаунчер Minecraft 1.16.5 + Fabric + Warax Visuals. Интерфейс сделан на HTML/CSS: стекло, «аврора», анимации, 10 тем. Он зашифрован и вшит прямо в `.exe`.

## Защита
- **Без входа ничего не работает.** Интерфейс игры открывается только после успешной проверки аккаунта на сервере.
- **Мод скачивается только после входа.** Ссылка временная (на 5 минут) и выдаётся только для вашего токена и HWID. Jar сохраняется во временную папку и удаляется после закрытия игры.
- **Строки и ключи зашифрованы.** Ключ Supabase, ссылки и весь интерфейс зашифрованы в exe. Ключ шифрования генерируется заново при каждой сборке.
- **Привязка к ПК (HWID).** Аккаунт привязывается к первому компьютеру, с которого выполнен вход. Токен, скопированный на другой ПК, сразу аннулируется.
- **Удалённое отключение старых версий.** Задаётся через `min_version` в таблице `launcher_config`.
- **Защита от отладки.** Отладчик, DevTools, меню по правому клику и горячие клавиши отключены. Работает один экземпляр лаунчера.

> Клиентская защита на 100% невозможна. Главная защита в том, что всё важное проверяет сервер. Даже если кто-то вскроет exe, без аккаунта и своего HWID он ничего не получит.

## Настройка (один раз)
1. **Supabase → SQL Editor:** если база новая, запустите `supabase_setup.sql` (от 1.4), затем `supabase/update.sql`.
2. **Мод в приватном хранилище (рекомендуется):** откройте Storage, создайте bucket `mods` с выключенным Public и загрузите туда `warax-visuals.jar`. Затем выполните:
   `supabase functions deploy get-mod --no-verify-jwt`. Код функции лежит в `supabase/functions/get-mod`.
   Если этот шаг пропустить, лаунчер возьмёт мод из GitHub-релиза (так менее защищённо).
3. **GitHub → Settings → Secrets → Actions:** добавьте `SUPABASE_URL` и `SUPABASE_KEY` (ключ anon / publishable).
4. Загрузите проект в `nerrlyzzz/Warax-Launcher-V2`. Сборка запустится в Actions, а `WaraxLauncher.exe` появится в релизе `releases`.

## Управление
| Задача | SQL |
|---|---|
| Сбросить привязку к ПК | `update launcher_users set hwid = null where login='vasya';` |
| Разрешить вход с любого ПК | `update launcher_users set hwid_lock = false where login='vasya';` |
| Отключить старые версии | `update launcher_config set min_version='2.1.0';` |
| Забанить | `update launcher_users set banned=true, ban_reason='...' where login='vasya';` |

## Требования
Windows 10 или 11 x64. WebView2 уже есть в системе. Если его нет, лаунчер сам предложит скачать.
Windows 7 и 8 не поддерживаются: на них WebView2 больше не работает. Для них остаётся Python-лаунчер 1.4.

## Локальная сборка
```
nuget install Microsoft.Web.WebView2 -OutputDirectory packages -ExcludeVersion
set SUPABASE_URL=...
set SUPABASE_KEY=...
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   (в "x64 Native Tools Command Prompt")
cmake --build build
```
`-DWL_DEV=ON` включает DevTools и отключает защиту. Используйте этот флаг только для отладки.
