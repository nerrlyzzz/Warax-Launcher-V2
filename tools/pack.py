#!/usr/bin/env python3
# =====================================================================
#  pack.py — шифрует весь интерфейс (web/) и секреты в C++ заголовки.
#  Запускается на этапе сборки (CMake / GitHub Actions).
#  Ключ и seed'ы случайны при каждой сборке, поэтому два .exe никогда не одинаковы.
# =====================================================================
import os, sys, secrets, pathlib

# консоль Windows (cp1252) не умеет кириллицу — переключаем вывод на UTF-8
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

ROOT = pathlib.Path(__file__).resolve().parent.parent
WEB = ROOT / "web"
GEN = ROOT / "src"

# файлы интерфейса, вшиваемые в exe
WEB_FILES = [
    "index.html", "app.css", "app.js", "presets.json",
]

MIME = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".jpg": "image/jpeg",
    ".webp": "image/webp",
    ".woff2": "font/woff2",
    ".ttf": "font/ttf",
    ".ico": "image/x-icon",
}

KEY = secrets.token_bytes(32)


def xorshift_stream(seed, n):
    s = seed & 0xFFFFFFFF
    if s == 0:
        s = 0x9E3779B9
    for _ in range(n):
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= (s >> 17)
        s ^= (s << 5) & 0xFFFFFFFF
        s &= 0xFFFFFFFF
        yield s & 0xFF


def seal(data: bytes):
    seed = int.from_bytes(secrets.token_bytes(4), "little") or 0x1234567
    out = bytearray(len(data))
    for i, (b, k) in enumerate(zip(data, xorshift_stream(seed, len(data)))):
        out[i] = b ^ k ^ KEY[i % 32]
    return seed, bytes(out)


def carr(name, data: bytes):
    parts = ["static const unsigned char %s[] = {" % name]
    line = "    "
    for i, b in enumerate(data):
        line += "0x%02x," % b
        if (i + 1) % 16 == 0:
            parts.append(line)
            line = "    "
    if line.strip():
        parts.append(line)
    parts.append("};")
    return "\n".join(parts)


def sealed_block(macro, text: str):
    seed, data = seal(text.encode("utf-8"))
    if len(data) == 0:
        seed, data = seal(b"\x00")
        data = b""
    nm = "g_" + macro.lower()
    body = carr(nm, data if data else b"\x00")
    n = len(data)
    return body + "\nstatic const Sealed %s = { %s, %d, 0x%08xu };\n" % (macro, nm, n, seed)


def main():
    GEN.mkdir(parents=True, exist_ok=True)

    supa_url = os.environ.get("SUPABASE_URL", "").strip()
    supa_key = os.environ.get("SUPABASE_KEY", "").strip()
    version = os.environ.get("LAUNCHER_VERSION", "2.0.0").strip() or "2.0.0"
    update_page = os.environ.get(
        "UPDATE_PAGE", "https://github.com/warvark/Warvex-Launcher/releases/tag/releases"
    ).strip()
    update_api = os.environ.get(
        "UPDATE_API",
        "https://api.github.com/repos/warvark/Warvex-Launcher/releases/tags/releases",
    ).strip()

    # ---------------- secrets.h ----------------
    s = []
    s.append("// АВТОСГЕНЕРИРОВАНО tools/pack.py — НЕ РЕДАКТИРОВАТЬ, НЕ КОММИТИТЬ РЕАЛЬНЫЕ КЛЮЧИ")
    s.append("#pragma once")
    s.append("#include <cstddef>")
    s.append("#include <cstdint>")
    s.append('#define WL_VERSION "%s"' % version.replace('"', ''))
    s.append("struct Sealed { const unsigned char* d; size_t n; uint32_t seed; };")
    s.append(carr("WL_K", KEY))
    s.append(sealed_block("WL_SUPA_URL", supa_url))
    s.append(sealed_block("WL_SUPA_KEY", supa_key))
    s.append(sealed_block("WL_UPDATE_PAGE", update_page))
    s.append(sealed_block("WL_UPDATE_API", update_api))
    (GEN / "secrets.h").write_text("\n".join(s) + "\n", encoding="utf-8")

    # ---------------- web_pack.h ----------------
    w = []
    w.append("// АВТОСГЕНЕРИРОВАНО tools/pack.py — интерфейс в зашифрованном виде")
    w.append("#pragma once")
    w.append("#include <cstddef>")
    w.append("#include <cstdint>")
    w.append("struct WebFile { const char* path; const wchar_t* mime; const unsigned char* d; size_t n; uint32_t seed; };")
    entries = []
    for idx, rel in enumerate(WEB_FILES):
        fp = WEB / rel
        if not fp.exists():
            print("pack.py: пропущен (нет файла):", rel, file=sys.stderr)
            continue
        data = fp.read_bytes()
        seed, enc = seal(data)
        nm = "g_web_%d" % idx
        w.append(carr(nm, enc))
        mime = MIME.get(fp.suffix.lower(), "application/octet-stream")
        entries.append('    { "%s", L"%s", %s, %d, 0x%08xu },' % (rel, mime, nm, len(enc), seed))
    w.append("static const WebFile WL_FILES[] = {")
    w.extend(entries)
    w.append("};")
    w.append("static const unsigned WL_FILE_COUNT = sizeof(WL_FILES)/sizeof(WL_FILES[0]);")
    (GEN / "web_pack.h").write_text("\n".join(w) + "\n", encoding="utf-8")

    print("pack.py: сгенерировано secrets.h и web_pack.h; файлов интерфейса:", len(entries),
          "| supabase:", "да" if supa_url and supa_key else "нет (задайте Secrets)")


if __name__ == "__main__":
    main()
