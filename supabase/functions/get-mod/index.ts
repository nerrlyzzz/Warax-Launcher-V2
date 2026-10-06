// Supabase Edge Function: get-mod
// Выдаёт временную (5 мин) ссылку на мод из ПРИВАТНОГО bucket 'mods' — только для валидной сессии
// с того же ПК (HWID). Деплой:  supabase functions deploy get-mod --no-verify-jwt
import { createClient } from "jsr:@supabase/supabase-js@2";

const db = createClient(Deno.env.get("SUPABASE_URL")!, Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!, {
  auth: { persistSession: false },
});

const out = (o: unknown, status = 200) =>
  new Response(JSON.stringify(o), { status, headers: { "Content-Type": "application/json" } });

Deno.serve(async (req) => {
  if (req.method !== "POST") return out({ ok: false, error: "method" }, 405);
  let body: { token?: string; hwid?: string; version?: string } = {};
  try { body = await req.json(); } catch { return out({ ok: false, error: "bad" }, 400); }
  const { token, hwid, version } = body;
  if (!token || !hwid) return out({ ok: false, error: "invalid" }, 401);

  // проверка сессии теми же правилами, что и вход (бан, срок, HWID, версия)
  const { data: chk, error } = await db.rpc("launcher_check", { p_token: token, p_hwid: hwid, p_version: version ?? "0" });
  if (error || !chk?.ok) return out({ ok: false, error: chk?.error ?? "invalid" }, 403);

  const { data: cfg } = await db.from("launcher_config").select("mod_path").eq("id", 1).single();
  const path = cfg?.mod_path ?? "warax-visuals.jar";
  const { data: signed, error: sErr } = await db.storage.from("mods").createSignedUrl(path, 300);
  if (sErr || !signed) return out({ ok: false, error: "nofile" }, 500);
  return out({ ok: true, url: signed.signedUrl });
});
