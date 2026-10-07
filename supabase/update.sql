-- =====================================================================
--  Warax Launcher 2.0 (C++) — обновление базы Supabase
--  1) Если база пустая — сначала запусти supabase_setup.sql от лаунчера 1.4.
--  2) Потом вставь ЭТОТ файл целиком в SQL Editor → Run. Можно запускать повторно.
--  Добавляет: привязку аккаунта к ПК (HWID), удалённое отключение старых версий,
--  выдачу мода только после входа.
-- =====================================================================

create extension if not exists pgcrypto with schema extensions;

alter table public.launcher_users    add column if not exists hwid text;          -- пусто = привяжется при первом входе
alter table public.launcher_users    add column if not exists hwid_lock boolean not null default true;
alter table public.launcher_sessions add column if not exists hwid text;

-- ---------- Настройки лаунчера (одна строка) ----------
create table if not exists public.launcher_config (
  id             int primary key default 1 check (id = 1),
  min_version    text not null default '2.0.0',   -- версии ниже не смогут войти
  latest_version text not null default '2.0.0',
  update_page    text not null default 'https://github.com/nerrlyzzz/Warax-Launcher-V2/releases/tag/releases',
  news           text,
  mod_path       text not null default 'warax-visuals.jar'  -- файл в приватном bucket 'mods'
);
insert into public.launcher_config(id) values (1) on conflict (id) do nothing;
alter table public.launcher_config enable row level security;
revoke all on public.launcher_config from anon, authenticated;

-- сравнение версий '2.0.10' > '2.0.9'
create or replace function public.launcher_ver(v text)
returns int[] language sql immutable as $$
  select coalesce(array_agg(coalesce(nullif(regexp_replace(x, '\D', '', 'g'), '')::int, 0)), '{0}')
  from unnest(string_to_array(coalesce(v, '0'), '.')) x;
$$;

create or replace function public.launcher_outdated(p_version text)
returns json language plpgsql security definer set search_path = public as $$
declare c launcher_config;
begin
  select * into c from launcher_config where id = 1;
  if launcher_ver(p_version) < launcher_ver(c.min_version) then
    return json_build_object('ok', false, 'error', 'outdated',
                             'min_version', c.min_version, 'update_page', c.update_page);
  end if;
  return null;
end $$;

-- старые версии функций (без HWID) удаляем — старый Python-лаунчер больше не войдёт
drop function if exists public.launcher_login(text, text);
drop function if exists public.launcher_check(text);

-- ---------- Вход ----------
create or replace function public.launcher_login(p_login text, p_password text, p_hwid text, p_version text)
returns json language plpgsql security definer
set search_path = public, extensions as $$
declare
  u launcher_users;
  t text;
  o json;
begin
  o := launcher_outdated(p_version);
  if o is not null then return o; end if;
  if coalesce(length(p_hwid), 0) < 16 then
    return json_build_object('ok', false, 'error', 'hwid');
  end if;

  select * into u from launcher_users where login = lower(trim(p_login));
  if not found then
    perform pg_sleep(0.4);
    return json_build_object('ok', false, 'error', 'wrong');
  end if;
  if u.locked_until is not null and u.locked_until > now() then
    return json_build_object('ok', false, 'error', 'locked', 'until', u.locked_until);
  end if;
  if u.password <> crypt(coalesce(p_password, ''), u.password) then
    update launcher_users set
      failed_attempts = case when failed_attempts + 1 >= 5 then 0 else failed_attempts + 1 end,
      locked_until    = case when failed_attempts + 1 >= 5 then now() + interval '5 minutes' else locked_until end
    where id = u.id;
    return json_build_object('ok', false, 'error', 'wrong');
  end if;
  if u.banned then
    return json_build_object('ok', false, 'error', 'banned', 'reason', u.ban_reason);
  end if;
  if u.expires_at is not null and u.expires_at <= now() then
    return json_build_object('ok', false, 'error', 'expired', 'until', u.expires_at);
  end if;

  -- привязка к ПК: первый вход запоминает HWID, с другого ПК — отказ
  if u.hwid_lock then
    if u.hwid is null then
      update launcher_users set hwid = p_hwid where id = u.id;
    elsif u.hwid <> p_hwid then
      return json_build_object('ok', false, 'error', 'hwid');
    end if;
  end if;

  update launcher_users set failed_attempts = 0, locked_until = null, last_login = now() where id = u.id;

  t := encode(gen_random_bytes(32), 'hex');
  insert into launcher_sessions(token, user_id, hwid) values (t, u.id, p_hwid);
  delete from launcher_sessions where user_id = u.id and token not in (
    select token from launcher_sessions where user_id = u.id order by last_seen desc limit 5);

  return json_build_object('ok', true, 'token', t, 'login', u.login,
                           'nick', u.nick, 'expires_at', u.expires_at);
end $$;

-- ---------- Проверка сессии ----------
create or replace function public.launcher_check(p_token text, p_hwid text, p_version text)
returns json language plpgsql security definer
set search_path = public as $$
declare
  u launcher_users;
  sh text;
  o json;
begin
  o := launcher_outdated(p_version);
  if o is not null then return o; end if;
  select usr.* into u from launcher_sessions s
    join launcher_users usr on usr.id = s.user_id where s.token = p_token;
  select s.hwid into sh from launcher_sessions s where s.token = p_token;
  if u.id is null then
    return json_build_object('ok', false, 'error', 'invalid');
  end if;
  if sh is distinct from p_hwid or (u.hwid_lock and u.hwid is distinct from p_hwid) then
    delete from launcher_sessions where token = p_token;   -- токен украли на другой ПК
    return json_build_object('ok', false, 'error', 'hwid');
  end if;
  if u.banned then
    return json_build_object('ok', false, 'error', 'banned', 'reason', u.ban_reason);
  end if;
  if u.expires_at is not null and u.expires_at <= now() then
    return json_build_object('ok', false, 'error', 'expired', 'until', u.expires_at);
  end if;
  update launcher_sessions set last_seen = now() where token = p_token;
  return json_build_object('ok', true, 'login', u.login, 'nick', u.nick, 'expires_at', u.expires_at);
end $$;

-- ---------- Инфо (новости / последняя версия) ----------
create or replace function public.launcher_info(p_hwid text default null, p_version text default null)
returns json language sql security definer set search_path = public as $$
  select json_build_object('ok', true, 'latest_version', latest_version, 'min_version', min_version,
                           'update_page', update_page, 'news', news)
  from launcher_config where id = 1;
$$;

revoke all on function public.launcher_login(text, text, text, text) from public;
revoke all on function public.launcher_check(text, text, text)       from public;
revoke all on function public.launcher_info(text, text)              from public;
revoke all on function public.launcher_outdated(text)                from public, anon, authenticated;
grant execute on function public.launcher_login(text, text, text, text) to anon;
grant execute on function public.launcher_check(text, text, text)       to anon;
grant execute on function public.launcher_info(text, text)              to anon;
grant execute on function public.launcher_logout(text)                  to anon;

-- ---------- Шпаргалка ----------
-- Сбросить привязку к ПК (друг поменял комп):
--   update launcher_users set hwid = null where login = 'vasya';
-- Разрешить вход с любого ПК:
--   update launcher_users set hwid_lock = false where login = 'vasya';
-- Отключить все версии ниже 2.1.0:
--   update launcher_config set min_version = '2.1.0', latest_version = '2.1.0';
