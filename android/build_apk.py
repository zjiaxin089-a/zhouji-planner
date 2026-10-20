from __future__ import annotations

import argparse
import base64
import hashlib
import os
import shutil
import struct
import subprocess
import tempfile
import time
import zipfile
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parent
APP_ID = "com.zhouji.planner"


def align(data: bytearray, n: int = 4) -> None:
    data.extend(b"\0" * ((-len(data)) % n))


def uleb(value: int) -> bytes:
    out = bytearray()
    while True:
        b = value & 0x7F
        value >>= 7
        out.append(b | (0x80 if value else 0))
        if not value:
            return bytes(out)


def utf8_len(value: int) -> bytes:
    if value <= 0x7F:
        return bytes([value])
    return bytes([0x80 | (value >> 8), value & 0xFF])


class StringPool:
    def __init__(self) -> None:
        self.values: list[str] = []
        self.index: dict[str, int] = {}

    def add(self, value: str) -> int:
        if value not in self.index:
            self.index[value] = len(self.values)
            self.values.append(value)
        return self.index[value]

    def __getitem__(self, value: str) -> int:
        return self.index[value]

    def build(self) -> bytes:
        payload = bytearray()
        offsets: list[int] = []
        for value in self.values:
            raw = value.encode("utf-8")
            offsets.append(len(payload))
            payload += utf8_len(len(value)) + utf8_len(len(raw)) + raw + b"\0"
        header_size = 28
        strings_start = header_size + 4 * len(offsets)
        chunk_size = strings_start + len(payload)
        chunk_size = (chunk_size + 3) & ~3
        out = bytearray(struct.pack("<HHIIIIII", 0x0001, header_size, chunk_size,
                                    len(offsets), 0, 0x100, strings_start, 0))
        out += struct.pack("<" + "I" * len(offsets), *offsets) if offsets else b""
        out += payload
        out += b"\0" * (chunk_size - len(out))
        return bytes(out)


def build_manifest() -> bytes:
    sp = StringPool()
    names = [
        "android", "http://schemas.android.com/apk/res/android", "manifest",
        "uses-sdk", "uses-permission", "application", "activity", "intent-filter",
        "action", "category", "versionCode", "versionName", "minSdkVersion",
        "targetSdkVersion", "name", "label", "icon", "exported", "package", APP_ID, "1.1",
        "android.permission.INTERNET", "周迹", ".MainActivity",
        "android.intent.action.MAIN", "android.intent.category.LAUNCHER",
    ]
    for value in names:
        sp.add(value)

    android_uri = sp["http://schemas.android.com/apk/res/android"]
    no = 0xFFFFFFFF
    attr_res = {
        "versionCode": 0x0101021B,
        "versionName": 0x0101021C,
        "minSdkVersion": 0x0101020C,
        "targetSdkVersion": 0x01010270,
        "name": 0x01010003,
        "label": 0x01010001,
        "icon": 0x01010002,
        "exported": 0x0101054B,
    }

    def node_header(kind: int, size: int, line: int = 1) -> bytes:
        return struct.pack("<HHIII", kind, 16, size, line, no)

    def namespace(kind: int) -> bytes:
        return node_header(kind, 24) + struct.pack("<II", sp["android"], android_uri)

    def attr(name: str, value, data_type: int = 0x03, android_ns: bool = True) -> bytes:
        if data_type == 0x03:
            raw = sp.add(str(value))
            data = raw
        else:
            raw = no
            data = int(value)
        return struct.pack("<IIIHBBI", android_uri if android_ns else no, sp[name], raw, 8, 0, data_type, data)

    def start(name: str, attrs: list[bytes] | None = None) -> bytes:
        attrs = attrs or []
        size = 36 + 20 * len(attrs)
        ext = struct.pack("<IIHHHHHH", no, sp[name], 20, 20, len(attrs), 0, 0, 0)
        return node_header(0x0102, size) + ext + b"".join(attrs)

    def end(name: str) -> bytes:
        return node_header(0x0103, 24) + struct.pack("<II", no, sp[name])

    body = bytearray()
    body += namespace(0x0100)
    body += start("manifest", [attr("package", APP_ID, android_ns=False), attr("versionCode", 2, 0x10), attr("versionName", "1.1")])
    body += start("uses-sdk", [attr("minSdkVersion", 23, 0x10), attr("targetSdkVersion", 28, 0x10)])
    body += end("uses-sdk")
    body += start("uses-permission", [attr("name", "android.permission.INTERNET")])
    body += end("uses-permission")
    body += start("application", [attr("label", "周迹"), attr("icon", 0x7F010000, 0x01)])
    body += start("activity", [attr("name", ".MainActivity"), attr("exported", 1, 0x12)])
    body += start("intent-filter")
    body += start("action", [attr("name", "android.intent.action.MAIN")])
    body += end("action")
    body += start("category", [attr("name", "android.intent.category.LAUNCHER")])
    body += end("category")
    body += end("intent-filter")
    body += end("activity")
    body += end("application")
    body += end("manifest")
    body += namespace(0x0101)

    pool = sp.build()
    resource_ids = [0] * len(sp.values)
    for name, resource_id in attr_res.items():
        resource_ids[sp[name]] = resource_id
    resource_map = struct.pack("<HHI", 0x0180, 8, 8 + 4 * len(resource_ids))
    resource_map += struct.pack("<" + "I" * len(resource_ids), *resource_ids)
    size = 8 + len(pool) + len(resource_map) + len(body)
    return struct.pack("<HHI", 0x0003, 8, size) + pool + resource_map + body


def build_resources() -> bytes:
    values = StringPool()
    values.add("res/drawable/icon.png")
    value_pool = values.build()

    type_names = StringPool()
    type_names.add("drawable")
    type_pool = type_names.build()
    key_names = StringPool()
    key_names.add("icon")
    key_pool = key_names.build()

    type_spec = struct.pack("<HHIBBHI", 0x0202, 16, 20, 1, 0, 0, 1)
    type_spec += struct.pack("<I", 0)

    config = struct.pack("<I", 64) + b"\0" * 60
    type_header_size = 20 + len(config)
    entries_start = type_header_size + 4
    entry = struct.pack("<HHI", 8, 0, 0)
    entry += struct.pack("<HBBI", 8, 0, 0x03, 0)
    type_chunk_size = entries_start + len(entry)
    type_chunk = struct.pack("<HHIBBHII", 0x0201, type_header_size, type_chunk_size,
                             1, 0, 0, 1, entries_start)
    type_chunk += config + struct.pack("<I", 0) + entry

    package_header_size = 288
    type_strings_off = package_header_size
    key_strings_off = type_strings_off + len(type_pool)
    package_size = package_header_size + len(type_pool) + len(key_pool) + len(type_spec) + len(type_chunk)
    package_name = "zhouji".encode("utf-16-le") + b"\0\0"
    package_name += b"\0" * (256 - len(package_name))
    package = struct.pack("<HHII", 0x0200, package_header_size, package_size, 0x7F)
    package += package_name
    package += struct.pack("<IIIII", type_strings_off, 1, key_strings_off, 1, 0)
    package += type_pool + key_pool + type_spec + type_chunk

    total_size = 12 + len(value_pool) + len(package)
    return struct.pack("<HHII", 0x0002, 12, total_size, 1) + value_pool + package


def build_dex() -> bytes:
    type_desc = [
        "Landroid/app/Activity;", "Landroid/content/Context;", "Landroid/os/Bundle;",
        "Landroid/view/View;", "Landroid/webkit/WebSettings;", "Landroid/webkit/WebView;",
        "Lcom/zhouji/planner/MainActivity;", "Ljava/lang/String;", "V", "Z",
    ]
    proto_defs = [
        ("V", "V", ()),
        ("VL", "V", ("Landroid/os/Bundle;",)),
        ("VL", "V", ("Landroid/content/Context;",)),
        ("L", "Landroid/webkit/WebSettings;", ()),
        ("VZ", "V", ("Z",)),
        ("VL", "V", ("Ljava/lang/String;",)),
        ("VL", "V", ("Landroid/view/View;",)),
    ]
    method_defs = [
        ("Landroid/app/Activity;", "<init>", ("V", "V", ())),
        ("Landroid/app/Activity;", "onCreate", ("VL", "V", ("Landroid/os/Bundle;",))),
        ("Landroid/app/Activity;", "setContentView", ("VL", "V", ("Landroid/view/View;",))),
        ("Landroid/webkit/WebSettings;", "setDomStorageEnabled", ("VZ", "V", ("Z",))),
        ("Landroid/webkit/WebSettings;", "setJavaScriptEnabled", ("VZ", "V", ("Z",))),
        ("Landroid/webkit/WebView;", "<init>", ("VL", "V", ("Landroid/content/Context;",))),
        ("Landroid/webkit/WebView;", "getSettings", ("L", "Landroid/webkit/WebSettings;", ())),
        ("Landroid/webkit/WebView;", "loadUrl", ("VL", "V", ("Ljava/lang/String;",))),
        ("Lcom/zhouji/planner/MainActivity;", "<init>", ("V", "V", ())),
        ("Lcom/zhouji/planner/MainActivity;", "onCreate", ("VL", "V", ("Landroid/os/Bundle;",))),
    ]
    extra_strings = ["<init>", "onCreate", "setContentView", "setDomStorageEnabled",
                     "setJavaScriptEnabled", "getSettings", "loadUrl",
                     "file:///android_asset/index.html", "L", "VL", "VZ"]
    strings = sorted(set(type_desc + extra_strings))
    sidx = {s: i for i, s in enumerate(strings)}
    types = sorted(type_desc, key=lambda x: sidx[x])
    tidx = {s: i for i, s in enumerate(types)}
    protos = sorted(set(proto_defs), key=lambda p: (tidx[p[1]], tuple(tidx[x] for x in p[2])))
    pidx = {p: i for i, p in enumerate(protos)}
    methods = sorted(method_defs, key=lambda m: (tidx[m[0]], sidx[m[1]], pidx[m[2]]))
    midx = {m: i for i, m in enumerate(methods)}

    header_size = 0x70
    string_ids_off = header_size
    type_ids_off = string_ids_off + 4 * len(strings)
    proto_ids_off = type_ids_off + 4 * len(types)
    method_ids_off = proto_ids_off + 12 * len(protos)
    class_defs_off = method_ids_off + 8 * len(methods)
    data_off = class_defs_off + 32
    data = bytearray()

    string_offsets: list[int] = []
    for value in strings:
        string_offsets.append(data_off + len(data))
        raw = value.encode("utf-8")
        data += uleb(len(value)) + raw + b"\0"

    align(data)
    type_list_offsets: dict[tuple[str, ...], int] = {}
    for params in sorted({p[2] for p in protos if p[2]}, key=lambda p: tuple(tidx[x] for x in p)):
        align(data)
        type_list_offsets[params] = data_off + len(data)
        data += struct.pack("<I", len(params))
        for item in params:
            data += struct.pack("<H", tidx[item])
        align(data)

    def invoke(op: int, registers: list[int], method: tuple) -> list[int]:
        count = len(registers)
        regs = registers + [0] * (5 - count)
        c, d, e, f, g = regs[:5]
        return [op | (g << 8) | (count << 12), midx[method], c | (d << 4) | (e << 8) | (f << 12)]

    def code_item(registers: int, ins: int, outs: int, words: list[int]) -> bytes:
        return struct.pack("<HHHHII", registers, ins, outs, 0, 0, len(words)) + struct.pack("<" + "H" * len(words), *words)

    activity_init = ("Landroid/app/Activity;", "<init>", ("V", "V", ()))
    activity_oncreate = ("Landroid/app/Activity;", "onCreate", ("VL", "V", ("Landroid/os/Bundle;",)))
    activity_setview = ("Landroid/app/Activity;", "setContentView", ("VL", "V", ("Landroid/view/View;",)))
    webview_init = ("Landroid/webkit/WebView;", "<init>", ("VL", "V", ("Landroid/content/Context;",)))
    webview_getsettings = ("Landroid/webkit/WebView;", "getSettings", ("L", "Landroid/webkit/WebSettings;", ()))
    settings_js = ("Landroid/webkit/WebSettings;", "setJavaScriptEnabled", ("VZ", "V", ("Z",)))
    settings_dom = ("Landroid/webkit/WebSettings;", "setDomStorageEnabled", ("VZ", "V", ("Z",)))
    webview_load = ("Landroid/webkit/WebView;", "loadUrl", ("VL", "V", ("Ljava/lang/String;",)))

    init_words = invoke(0x70, [0], activity_init) + [0x000E]
    oncreate_words: list[int] = []
    oncreate_words += invoke(0x6F, [3, 4], activity_oncreate)
    oncreate_words += [0x0022, tidx["Landroid/webkit/WebView;"]]
    oncreate_words += invoke(0x70, [0, 3], webview_init)
    oncreate_words += invoke(0x6E, [0], webview_getsettings)
    oncreate_words += [0x010C]
    oncreate_words += [0x1212]
    oncreate_words += invoke(0x6E, [1, 2], settings_js)
    oncreate_words += invoke(0x6E, [1, 2], settings_dom)
    oncreate_words += [0x011A, sidx["file:///android_asset/index.html"]]
    oncreate_words += invoke(0x6E, [0, 1], webview_load)
    oncreate_words += invoke(0x6E, [3, 0], activity_setview)
    oncreate_words += [0x000E]

    align(data)
    init_code_off = data_off + len(data)
    data += code_item(1, 1, 1, init_words)
    align(data)
    oncreate_code_off = data_off + len(data)
    data += code_item(5, 2, 2, oncreate_words)

    main_init = ("Lcom/zhouji/planner/MainActivity;", "<init>", ("V", "V", ()))
    main_oncreate = ("Lcom/zhouji/planner/MainActivity;", "onCreate", ("VL", "V", ("Landroid/os/Bundle;",)))
    class_data_off = data_off + len(data)
    class_data = bytearray(uleb(0) + uleb(0) + uleb(1) + uleb(1))
    class_data += uleb(midx[main_init]) + uleb(0x10001) + uleb(init_code_off)
    class_data += uleb(midx[main_oncreate]) + uleb(0x1) + uleb(oncreate_code_off)
    data += class_data

    align(data)
    map_off = data_off + len(data)
    map_entries = [
        (0x0000, 1, 0), (0x0001, len(strings), string_ids_off),
        (0x0002, len(types), type_ids_off), (0x0003, len(protos), proto_ids_off),
        (0x0005, len(methods), method_ids_off), (0x0006, 1, class_defs_off),
        (0x2002, len(strings), string_offsets[0]),
    ]
    if type_list_offsets:
        map_entries.append((0x1001, len(type_list_offsets), min(type_list_offsets.values())))
    map_entries += [(0x2001, 2, init_code_off), (0x2000, 1, class_data_off), (0x1000, 1, map_off)]
    map_entries.sort(key=lambda x: x[2])
    data += struct.pack("<I", len(map_entries))
    for kind, count, offset in map_entries:
        data += struct.pack("<HHII", kind, 0, count, offset)

    ids = bytearray()
    ids += b"".join(struct.pack("<I", x) for x in string_offsets)
    ids += b"".join(struct.pack("<I", sidx[x]) for x in types)
    for shorty, ret, params in protos:
        ids += struct.pack("<III", sidx[shorty], tidx[ret], type_list_offsets.get(params, 0))
    for cls, name, proto in methods:
        ids += struct.pack("<HHI", tidx[cls], pidx[proto], sidx[name])
    ids += struct.pack("<IIIIIIII", tidx["Lcom/zhouji/planner/MainActivity;"], 0x1,
                       tidx["Landroid/app/Activity;"], 0, 0xFFFFFFFF, 0, class_data_off, 0)

    file_size = header_size + len(ids) + len(data)
    header = bytearray(b"dex\n035\0" + b"\0" * 24)
    header += struct.pack("<20I", file_size, header_size, 0x12345678, 0, 0, map_off,
                          len(strings), string_ids_off, len(types), type_ids_off,
                          len(protos), proto_ids_off, 0, 0, len(methods), method_ids_off,
                          1, class_defs_off, len(data), data_off)
    dex = header + ids + data
    dex[12:32] = hashlib.sha1(dex[32:]).digest()
    dex[8:12] = struct.pack("<I", zlib.adler32(dex[12:]) & 0xFFFFFFFF)
    return bytes(dex)


def wrap_header(name: str, value: str) -> bytes:
    raw = f"{name}: {value}".encode("utf-8")
    lines = []
    first = True
    while raw:
        limit = 72 if first else 71
        lines.append((b"" if first else b" ") + raw[:limit])
        raw = raw[limit:]
        first = False
    return b"\r\n".join(lines) + b"\r\n"


def signature_files(entries: dict[str, bytes]) -> tuple[bytes, bytes]:
    main = wrap_header("Manifest-Version", "1.0") + wrap_header("Created-By", "Zhouji Local Builder") + b"\r\n"
    sections = {}
    manifest = bytearray(main)
    for name in sorted(entries):
        section = wrap_header("Name", name)
        digest = base64.b64encode(hashlib.sha256(entries[name]).digest()).decode()
        section += wrap_header("SHA-256-Digest", digest) + b"\r\n"
        sections[name] = section
        manifest += section
    sf = bytearray(wrap_header("Signature-Version", "1.0") + wrap_header("Created-By", "Zhouji Local Builder"))
    sf += wrap_header("SHA-256-Digest-Manifest", base64.b64encode(hashlib.sha256(manifest).digest()).decode())
    sf += wrap_header("SHA-256-Digest-Manifest-Main-Attributes", base64.b64encode(hashlib.sha256(main).digest()).decode()) + b"\r\n"
    for name in sorted(entries):
        sf += wrap_header("Name", name)
        sf += wrap_header("SHA-256-Digest", base64.b64encode(hashlib.sha256(sections[name]).digest()).decode()) + b"\r\n"
    return bytes(manifest), bytes(sf)


def find_openssl() -> str:
    options = [shutil.which("openssl"), r"D:\msys64\mingw64\bin\openssl.exe"]
    for option in options:
        if option and Path(option).exists():
            return str(option)
    raise RuntimeError("OpenSSL not found")


def ensure_certificate(signing: Path) -> tuple[Path, Path]:
    signing.mkdir(parents=True, exist_ok=True)
    key = signing / "zhouji-release-key.pem"
    cert = signing / "zhouji-release-cert.pem"
    if key.exists() and cert.exists() and key.stat().st_size > 100 and cert.stat().st_size > 100:
        return key, cert
    subprocess.run([find_openssl(), "req", "-x509", "-newkey", "rsa:2048", "-sha256",
                    "-keyout", str(key), "-out", str(cert), "-days", "3650", "-nodes",
                    "-subj", "/CN=Zhouji Generic/O=Local Build/C=CN"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return key, cert


def build_apk(output: Path, icon_path: Path) -> None:
    if not icon_path.exists():
        raise RuntimeError(f"Icon file not found: {icon_path}")
    entries = {
        "AndroidManifest.xml": build_manifest(),
        "resources.arsc": build_resources(),
        "classes.dex": build_dex(),
        "assets/index.html": (ROOT / "assets" / "index.html").read_bytes(),
        "res/drawable/icon.png": icon_path.read_bytes(),
    }
    manifest, sf = signature_files(entries)
    key, cert = ensure_certificate(output.parent / ".zhouji-android-signing")
    with tempfile.TemporaryDirectory(dir=output.parent) as temp_name:
        temp = Path(temp_name)
        sf_path = temp / "CERT.SF"
        rsa_path = temp / "CERT.RSA"
        sf_path.write_bytes(sf)
        subprocess.run([find_openssl(), "smime", "-sign", "-binary", "-noattr",
                        "-in", str(sf_path), "-signer", str(cert), "-inkey", str(key),
                        "-outform", "DER", "-out", str(rsa_path)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        signed = {
            "META-INF/MANIFEST.MF": manifest,
            "META-INF/CERT.SF": sf,
            "META-INF/CERT.RSA": rsa_path.read_bytes(),
            **entries,
        }
        output.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for name, content in signed.items():
                info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o644 << 16
                archive.writestr(info, content)


def verify_apk(output: Path) -> None:
    with zipfile.ZipFile(output) as archive:
        required = {"AndroidManifest.xml", "resources.arsc", "classes.dex", "assets/index.html",
                    "res/drawable/icon.png",
                    "META-INF/MANIFEST.MF", "META-INF/CERT.SF", "META-INF/CERT.RSA"}
        missing = required - set(archive.namelist())
        if missing:
            raise RuntimeError(f"APK missing entries: {sorted(missing)}")
        dex = archive.read("classes.dex")
        if dex[:8] != b"dex\n035\0":
            raise RuntimeError("Invalid DEX magic")
        if struct.unpack("<I", dex[8:12])[0] != (zlib.adler32(dex[12:]) & 0xFFFFFFFF):
            raise RuntimeError("Invalid DEX checksum")
        if dex[12:32] != hashlib.sha1(dex[32:]).digest():
            raise RuntimeError("Invalid DEX signature")
        sf = archive.read("META-INF/CERT.SF")
        rsa = archive.read("META-INF/CERT.RSA")
        html = archive.read("assets/index.html").decode("utf-8")
        private_terms = ROOT / "private_terms.txt"
        forbidden = []
        if private_terms.exists():
            forbidden = [line.strip() for line in private_terms.read_text("utf-8").splitlines() if line.strip()]
        found = [x for x in forbidden if x in html]
        if found:
            raise RuntimeError(f"Private terms found: {found}")
        with tempfile.TemporaryDirectory(dir=output.parent) as temp_name:
            temp = Path(temp_name)
            (temp / "CERT.SF").write_bytes(sf)
            (temp / "CERT.RSA").write_bytes(rsa)
            subprocess.run([find_openssl(), "smime", "-verify", "-inform", "DER",
                            "-in", str(temp / "CERT.RSA"), "-content", str(temp / "CERT.SF"),
                            "-noverify", "-out", os.devnull], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", nargs="?", type=Path, default=ROOT.parent / "周迹通用版-Android.apk")
    parser.add_argument("--icon", type=Path)
    args = parser.parse_args()
    icon = args.icon or args.output.parent / "zhouji-icon-v2.png"
    build_apk(args.output, icon)
    verify_apk(args.output)
    digest = hashlib.sha256(args.output.read_bytes()).hexdigest().upper()
    print(f"APK={args.output.resolve()}")
    print(f"SIZE={args.output.stat().st_size}")
    print(f"SHA256={digest}")
    print(f"PACKAGE={APP_ID}")
    print("MIN_SDK=23")
    print("TARGET_SDK=28")
    print("PERMISSIONS=android.permission.INTERNET")
    print("VERIFY=PASS")


if __name__ == "__main__":
    main()
