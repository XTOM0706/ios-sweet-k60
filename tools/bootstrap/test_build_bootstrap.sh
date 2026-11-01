#!/bin/bash
# test_build_bootstrap.sh —— build_bootstrap.sh 的离线自测（不联网、不下载真 tarball）。
#
# 造一个假的 Procursus rootless bootstrap（var/jb 前缀，带 dpkg status 和
# info/*.list），跑一遍打包，然后检查:
#   - 前缀被改写成 var/k60bootstrap/
#   - 基础包 + Depends 闭包里的文件在
#   - 闭包外的包（zsh / man-db）和裁剪目录（man/doc）不在
#   - 解包后目录结构对、可执行位还在、附属文件齐
#
#   ./test_build_bootstrap.sh

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUILD="$SCRIPT_DIR/build_bootstrap.sh"

TMP=$(mktemp -d "${TMPDIR:-/tmp}/k60bs-selftest.XXXXXX")
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

fail=0
check() {  # check <描述> <命令...>
	local desc=$1
	shift
	if "$@" >/dev/null 2>&1; then
		printf '  ok   %s\n' "$desc"
	else
		printf '  FAIL %s\n' "$desc" >&2
		fail=$((fail + 1))
	fi
}
check_not() {
	local desc=$1
	shift
	if "$@" >/dev/null 2>&1; then
		printf '  FAIL %s\n' "$desc" >&2
		fail=$((fail + 1))
	else
		printf '  ok   %s\n' "$desc"
	fi
}

# ---------------------------------------------------------------------------
# 1. 造假的 bootstrap 树
# ---------------------------------------------------------------------------

ROOT="$TMP/src/var/jb"
mkdir -p "$ROOT"/{usr/bin,usr/lib/apt/methods,usr/share/dpkg/tuple,etc/apt,var/lib/dpkg/info}

put() {  # put <相对路径> [权限]
	local p="$ROOT/${1#/}"
	mkdir -p "$(dirname "$p")"
	printf '#!/bin/sh\necho %s\n' "$(basename "$p")" > "$p"
	chmod "${2:-0755}" "$p"
}

put /usr/bin/dpkg
put /usr/bin/apt
put /usr/bin/bash
put /usr/bin/ls
put /usr/bin/ldid
put /usr/bin/uicache
put /usr/bin/zsh                       # 不在闭包里 → 应该被丢掉
put /usr/lib/libc.so.6 0644
put /usr/lib/libapt-pkg.so.6.0 0644
put /usr/lib/libgnutls.so.30 0644
put /usr/lib/apt/methods/http
put /usr/share/dpkg/tuple/arch 0644
put /etc/apt/sources.list 0644
put /usr/share/man/man1/dpkg.1.gz 0644  # 裁剪规则 → 丢
put /usr/share/doc/dpkg/copyright 0644  # 裁剪规则 → 丢

# 各包的 .list（dpkg 记的绝对路径）
cat > "$ROOT/var/lib/dpkg/info/dpkg.list" <<'EOF'
/.
/usr
/usr/bin
/usr/bin/dpkg
/usr/share
/usr/share/dpkg
/usr/share/dpkg/tuple
/usr/share/dpkg/tuple/arch
/usr/share/man
/usr/share/man/man1
/usr/share/man/man1/dpkg.1.gz
/usr/share/doc
/usr/share/doc/dpkg
/usr/share/doc/dpkg/copyright
/var/lib/dpkg
/var/lib/dpkg/info
EOF
cat > "$ROOT/var/lib/dpkg/info/apt.list" <<'EOF'
/usr/bin/apt
/usr/lib/apt/methods/http
/etc/apt/sources.list
EOF
cat > "$ROOT/var/lib/dpkg/info/bash.list" <<'EOF'
/usr/bin/bash
EOF
cat > "$ROOT/var/lib/dpkg/info/coreutils.list" <<'EOF'
/usr/bin/ls
EOF
cat > "$ROOT/var/lib/dpkg/info/ldid.list" <<'EOF'
/usr/bin/ldid
EOF
cat > "$ROOT/var/lib/dpkg/info/uikittools.list" <<'EOF'
/usr/bin/uicache
EOF
cat > "$ROOT/var/lib/dpkg/info/libc6.list" <<'EOF'
/usr/lib/libc.so.6
EOF
cat > "$ROOT/var/lib/dpkg/info/libapt-pkg6.0.list" <<'EOF'
/usr/lib/libapt-pkg.so.6.0
EOF
cat > "$ROOT/var/lib/dpkg/info/libgnutls30.list" <<'EOF'
/usr/lib/libgnutls.so.30
EOF
cat > "$ROOT/var/lib/dpkg/info/zsh.list" <<'EOF'
/usr/bin/zsh
EOF
cat > "$ROOT/var/lib/dpkg/info/man-db.list" <<'EOF'
/usr/share/man/man1/dpkg.1.gz
EOF

cat > "$ROOT/var/lib/dpkg/status" <<'EOF'
Package: dpkg
Version: 1.22.6
Depends: libc6 (>= 2.34)

Package: apt
Version: 2.7.14
Pre-Depends: dpkg (>= 1.20)
Depends: libapt-pkg6.0, libgnutls30, adduser | coreutils

Package: bash
Version: 5.2.21
Depends: libc6

Package: coreutils
Version: 9.4
Depends: libc6

Package: ldid
Version: 2.1.5
Depends: libc6

Package: uikittools
Version: 2.0.0
Depends: libc6

Package: libc6
Version: 2.38

Package: libapt-pkg6.0
Version: 2.7.14
Depends: libc6

Package: libgnutls30
Version: 3.8.3
Depends: libc6

Package: zsh
Version: 5.9
Depends: libc6

Package: man-db
Version: 2.12
Depends: libc6
EOF

SRC_TAR="$TMP/bootstrap-iphoneos-arm64.tar.zst"
tar --zstd -cf "$SRC_TAR" -C "$TMP/src" .

# ---------------------------------------------------------------------------
# 2. dry-run：只打印，不写盘
# ---------------------------------------------------------------------------

OUT="$TMP/out"
echo "[dry-run]"
DRY_OUT=$("$BUILD" --src "$SRC_TAR" --out "$OUT" --dry-run 2>&1)
printf '%s\n' "$DRY_OUT" | sed 's/^/    /'
check "dry-run 报出保留文件数" grep -q '保留文件' <<<"$DRY_OUT"
check "dry-run 认出 rootless 前缀" grep -q '源前缀.*var/jb' <<<"$DRY_OUT"
check_not "dry-run 没建输出目录" test -e "$OUT"

# ---------------------------------------------------------------------------
# 3. 真打包
# ---------------------------------------------------------------------------

echo "[build]"
"$BUILD" --src "$SRC_TAR" --out "$OUT" 2>&1 | sed 's/^/    /'
OUT_TAR=$(ls "$OUT"/k60bootstrap-*.tar.zst)
LIST="$TMP/out.list"
tar -tf "$OUT_TAR" | sed 's,^\./,,' | sort -u > "$LIST"

echo "[检查产物]"
check "产物 tar 存在" test -s "$OUT_TAR"
check "env 片段存在" test -f "$OUT/k60bootstrap.env"
check "manifest 存在" test -f "$OUT/manifest.txt"
check "有 dpkg" grep -qx 'var/k60bootstrap/usr/bin/dpkg' "$LIST"
check "有 bash" grep -qx 'var/k60bootstrap/usr/bin/bash' "$LIST"
check "有 ldid" grep -qx 'var/k60bootstrap/usr/bin/ldid' "$LIST"
check "有 uicache" grep -qx 'var/k60bootstrap/usr/bin/uicache' "$LIST"
check "闭包带上了 libc6" grep -qx 'var/k60bootstrap/usr/lib/libc.so.6' "$LIST"
check "闭包带上了 libapt-pkg" grep -qx 'var/k60bootstrap/usr/lib/libapt-pkg.so.6.0' "$LIST"
check "备选依赖 adduser|coreutils 选中 coreutils" grep -qx 'var/k60bootstrap/usr/bin/ls' "$LIST"
check "dpkg status 在" grep -qx 'var/k60bootstrap/var/lib/dpkg/status' "$LIST"
check "dpkg info 目录在" grep -q '^var/k60bootstrap/var/lib/dpkg/info/' "$LIST"
check "空目录 updates 在" grep -qx 'var/k60bootstrap/var/lib/dpkg/updates/' "$LIST"
check "apt methods 在" grep -qx 'var/k60bootstrap/usr/lib/apt/methods/http' "$LIST"
check "apt sources.list 在" grep -qx 'var/k60bootstrap/etc/apt/sources.list' "$LIST"
check_not "闭包外的 zsh 被丢掉" grep -qx 'var/k60bootstrap/usr/bin/zsh' "$LIST"
check_not "man 页被丢掉" grep -q 'usr/share/man' "$LIST"
check_not "doc 被丢掉" grep -q 'usr/share/doc' "$LIST"
check_not "没有绝对路径成员" grep -q '^/' "$LIST"
check_not "没有残留 var/jb" grep -q 'var/jb' "$LIST"

echo "[解包检查]"
tar -xf "$OUT_TAR" -C "$TMP"
check "解到根下就是 /var/k60bootstrap" test -x "$TMP/var/k60bootstrap/usr/bin/dpkg"
check "可执行位保住了" test -x "$TMP/var/k60bootstrap/usr/bin/bash"
check "普通文件没被改成可执行" test ! -x "$TMP/var/k60bootstrap/usr/lib/libc.so.6"
check "dpkg admindir 存在" test -d "$TMP/var/k60bootstrap/var/lib/dpkg/info"

echo
if [[ $fail -eq 0 ]]; then
	echo "全部通过"
	exit 0
fi
echo "$fail 项失败" >&2
exit 1
