#!/bin/bash
# build_bootstrap.sh —— 里程碑 I1：Procursus bootstrap → 最小集 → 可解包的 tar.zst
#
# 做的事（全部离线可跑）:
#   1. 找到/下载 Procursus 的 bootstrap-iphoneos-arm64.tar.zst
#   2. 认出它用的是哪种前缀（rootful 的 usr/、rootless 的 var/jb/）
#   3. 从 tarball 里的 var/lib/dpkg/{status,info/*.list} 算"最小集":
#        基础包 (dpkg apt bash coreutils ldid uikittools) + 依赖闭包
#      再按裁剪规则丢掉 man/doc/locale/include 之类
#   4. 重打成 var/k60bootstrap/... 前缀的 tar.zst（设备上解到 / 即得
#      /var/k60bootstrap），另附 env 片段和清单
#
# 用法:
#   ./build_bootstrap.sh                       # 用本地已有的 tarball
#   ./build_bootstrap.sh --src ~/bs.tar.zst
#   ./build_bootstrap.sh --url https://...     # 缓存里没有才下载
#   ./build_bootstrap.sh --dry-run             # 只打印计划: 不下载、不写盘
#   ./build_bootstrap.sh --keep-extra zsh,git  # 额外保留几个包
#
# 产物（默认 tools/bootstrap/build/，.gitignore 已忽略）:
#   k60bootstrap-<日期>.tar.zst   tar -xf 它 -C / 就是 /var/k60bootstrap
#   k60bootstrap.env              source 一下: PATH / DPKG_ADMINDIR
#   manifest.txt                  保留了哪些包、丢了多少文件
#
# 不写 /etc、不要 sudo、不碰设备。

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

# Procursus 的 bootstrap 会换版本；换 URL 只改这里（或用 K60_BOOTSTRAP_URL / --url）
DEFAULT_URL=${K60_BOOTSTRAP_URL:-https://apt.procurs.us/bootstraps/1800/bootstrap-iphoneos-arm64.tar.zst}
TARGET_PREFIX=/var/k60bootstrap

# 最小集的基础包（其余靠 Depends 闭包补）。缺哪个只警告，不致命。
BASE_PACKAGES="dpkg apt bash coreutils ldid uikittools"

# 裁剪规则：这些路径一律不要（运行时用不上，占空间）
DROP_PATHS=(
	usr/share/man
	usr/share/doc
	usr/share/locale
	usr/share/info
	usr/share/gcc
	usr/share/terminfo
	usr/include
	usr/lib/debug
	usr/lib/gcc
	usr/share/perl5
	usr/lib/perl5
	usr/share/vim
)

SRC=""
URL=""
OUT_DIR="$SCRIPT_DIR/build"
DRY_RUN=0
WITH_DEPS=1
KEEP_EXTRA=""
KEEP_PATHS=""
VERBOSE=0

log()  { printf 'bootstrap: %s\n' "$*"; }
warn() { printf 'bootstrap: 警告: %s\n' "$*" >&2; }
die()  { printf 'bootstrap: 错误: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

usage() { sed -n '2,30p' "${BASH_SOURCE[0]}"; exit 2; }

while [[ $# -gt 0 ]]; do
	case $1 in
	--src)        SRC=${2:?--src 需要参数}; shift 2 ;;
	--url)        URL=${2:?--url 需要参数}; shift 2 ;;
	--out)        OUT_DIR=${2:?--out 需要参数}; shift 2 ;;
	--keep-extra) KEEP_EXTRA=${2:?--keep-extra 需要参数}; shift 2 ;;
	--keep-path)  KEEP_PATHS="${KEEP_PATHS} ${2:?--keep-path 需要参数}"; shift 2 ;;
	--no-deps)    WITH_DEPS=0; shift ;;
	--dry-run)    DRY_RUN=1; shift ;;
	-v|--verbose) VERBOSE=1; shift ;;
	-h|--help)    usage ;;
	*)            die "未知参数 $1（--help 看用法）" ;;
	esac
done

# ---------------------------------------------------------------------------
# 1. 找源 tarball
# ---------------------------------------------------------------------------

find_source() {
	if [[ -n $SRC ]]; then
		[[ -f $SRC ]] || die "--src 指的文件不存在: $SRC"
		printf '%s\n' "$SRC"
		return 0
	fi
	local c
	for c in \
		"${K60_BOOTSTRAP_SRC:-}" \
		"$SCRIPT_DIR/cache/bootstrap-iphoneos-arm64.tar.zst" \
		"$SCRIPT_DIR/bootstrap-iphoneos-arm64.tar.zst" \
		"$PWD/bootstrap-iphoneos-arm64.tar.zst" \
		"$HOME/hackios/artifacts/bootstrap/bootstrap-iphoneos-arm64.tar.zst" \
		"$HOME/hackios/artifacts/bootstrap-iphoneos-arm64.tar.zst"; do
		if [[ -n $c && -f $c ]]; then
			printf '%s\n' "$c"
			return 0
		fi
	done
	return 1
}

download() {
	local url=$1 dest=$2
	mkdir -p "$(dirname "$dest")"
	if have curl; then
		curl -fL --retry 3 -o "$dest.part" "$url"
	elif have wget; then
		wget -O "$dest.part" "$url"
	else
		die "要下载 $url，但 curl/wget 都没有"
	fi
	mv "$dest.part" "$dest"
	log "下载完成: $dest ($(du -h "$dest" | cut -f1))"
}

# ---------------------------------------------------------------------------
# 2. tarball 里的前缀 / 包清单
# ---------------------------------------------------------------------------

# 打印 src_prefix，例如 "" 或 "var/jb" 或 "var/k60bootstrap"
detect_prefix() {
	local listing=$1 p
	for p in "var/jb" "var/k60bootstrap" ""; do
		if grep -qxF "${p:+$p/}usr/bin/dpkg" "$listing"; then
			printf '%s' "$p"
			return 0
		fi
	done
	# 退一步：找任何 usr/bin/dpkg
	if grep -qE '(^|/)usr/bin/dpkg$' "$listing"; then
		grep -E '(^|/)usr/bin/dpkg$' "$listing" | head -1 | sed 's,usr/bin/dpkg$,,' | sed 's,/$,,' | sed 's,^\./,,' | tr -d '\n'
		return 0
	fi
	return 1
}

# status → "pkg<TAB>dep1 dep2 ..."（Depends/Pre-Depends，含续行）
pkg_deps() {
	awk '
	BEGIN { RS = ""; FS = "\n" }
	{
		pkg = ""; deps = ""; want = 0
		for (i = 1; i <= NF; i++) {
			line = $i
			if (line ~ /^Package:/) {
				sub(/^Package:[ \t]*/, "", line); pkg = line; want = 0
			} else if (line ~ /^(Pre-)?Depends:/) {
				sub(/^(Pre-)?Depends:[ \t]*/, "", line)
				deps = (deps == "" ? line : deps ", " line); want = 1
			} else if (want && line ~ /^[ \t]/) {
				deps = deps " " line
			} else {
				want = 0
			}
		}
		if (pkg != "") print pkg "\t" deps
	}' "$1"
}

# 依赖闭包：从 base 出发，Depends 的每组备选里挑第一个"存在"的包
resolve_closure() {
	local status=$1
	shift
	local -A deps=() kept=()
	local -a queue=()
	local pkg line group alt chosen

	while IFS=$'\t' read -r pkg line; do
		deps["$pkg"]=$line
	done < <(pkg_deps "$status")

	for pkg in "$@"; do
		[[ -n ${deps[$pkg]+x} ]] || { warn "tarball 里没有包 $pkg（跳过）"; continue; }
		queue+=("$pkg")
	done

	local i=0
	while (( i < ${#queue[@]} )); do
		pkg=${queue[i]}; i=$((i + 1))
		[[ -n ${kept[$pkg]+x} ]] && continue
		kept["$pkg"]=1
		[[ -z ${deps[$pkg]:-} ]] && continue
		# "a (>= 1), b | c" → 按逗号切组，组内按 | 切备选
		local IFS=','
		for group in ${deps[$pkg]}; do
			chosen=""
			local IFS='|'
			for alt in $group; do
				# "libc6 (>= 2.34)" / "foo:any" / " foo " → "libc6" / "foo" / "foo"
				alt=$(printf '%s' "$alt" | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//; s/[[:space:]]*\(.*//; s/:.*$//')
				if [[ -n ${deps[$alt]+x} ]]; then chosen=$alt; break; fi
			done
			unset IFS
			[[ -n $chosen ]] && queue+=("$chosen")
		done
		unset IFS
	done

	printf '%s\n' "${!kept[@]}" | sort
}

# 包的 .list 里的绝对路径 → tarball 成员名
files_for_packages() {
	local admin=$1 prefix=$2
	shift 2
	local pkg list f
	for pkg in "$@"; do
		list="$admin/info/$pkg.list"
		[[ -f $list ]] || continue
		while IFS= read -r f; do
			[[ -z $f ]] && continue
			printf '%s%s\n' "${prefix:+$prefix}" "$f"
		done < "$list"
	done
}

is_dropped() {
	local member=$1 rel=${1#./} d
	rel=${rel#"${SRC_PREFIX:+$SRC_PREFIX/}"}
	rel=${rel%/}
	kept_by_path_rule "$member" && return 1
	for d in "${DROP_PATHS[@]}"; do
		[[ $rel == "$d" || $rel == "$d"/* ]] && return 0
	done
	return 1
}

kept_by_path_rule() {
	local rel=${1#./} p
	rel=${rel#"${SRC_PREFIX:+$SRC_PREFIX/}"}
	for p in $KEEP_PATHS; do
		[[ $rel == "${p#/}" || $rel == "${p#/}"/* ]] && return 0
	done
	return 1
}

# ---------------------------------------------------------------------------
# 3. 主流程
# ---------------------------------------------------------------------------

WORK=$(mktemp -d "${TMPDIR:-/tmp}/k60bootstrap.XXXXXX")
cleanup() {
	if [[ ${K60_BOOTSTRAP_KEEP_WORK:-0} = 1 ]]; then
		printf 'bootstrap: 调试: 保留 %s\n' "$WORK" >&2
		return 0
	fi
	rm -rf "$WORK"
}
trap cleanup EXIT

STAMP=$(date +%Y%m%d)
OUT_TAR="$OUT_DIR/k60bootstrap-$STAMP.tar.zst"

[[ -z $KEEP_EXTRA ]] || BASE_PACKAGES="$BASE_PACKAGES ${KEEP_EXTRA//,/ }"

SRC_PATH=""
if SRC_PATH=$(find_source); then
	:
else
	SRC_PATH=""
fi

DOWNLOAD_URL=${URL:-$DEFAULT_URL}

if [[ $DRY_RUN -eq 0 ]]; then
	have tar || die "没有 tar"
	have zstd || die "没有 zstd（Arch: sudo pacman -S zstd；Debian: sudo apt install zstd）"
	if [[ -z $SRC_PATH ]]; then
		[[ -n $URL || -n $DOWNLOAD_URL ]] || die "没给 --url"
		SRC_PATH="$SCRIPT_DIR/cache/$(basename "$DOWNLOAD_URL")"
		if [[ -f $SRC_PATH ]]; then
			log "缓存命中: $SRC_PATH"
		else
			log "本地没有 tarball，开始下载: $DOWNLOAD_URL"
			download "$DOWNLOAD_URL" "$SRC_PATH"
		fi
	fi
fi

log "源 tarball : ${SRC_PATH:-<无>（dry-run 只看计划）}"
log "目标前缀   : $TARGET_PREFIX"
log "输出       : $OUT_TAR"

SRC_PREFIX=""
LISTING=""
PKGS=""
FILELIST=""

if [[ -n $SRC_PATH ]]; then
	tar -tf "$SRC_PATH" 2>/dev/null > "$WORK/listing.raw" \
		|| die "读不了 tarball 的目录: $SRC_PATH"
	# raw 保留原始的 "./" 前缀（tar -x 按名字取成员时要用它），norm 用来比对
	sed 's,^\./,,' "$WORK/listing.raw" | grep -v '^$' | sort -u > "$WORK/listing.txt"
	LISTING="$WORK/listing.txt"
	LISTING_RAW="$WORK/listing.raw"
	log "源里的文件 : $(wc -l < "$LISTING")"

	if ! SRC_PREFIX=$(detect_prefix "$LISTING"); then
		die "tarball 里找不到 usr/bin/dpkg，认不出这是 bootstrap 包"
	fi
	log "源前缀     : ${SRC_PREFIX:-<无，rootful 布局>}"

	# dpkg 的管理目录：包清单/依赖都从这里读
	ADMIN="$WORK/admin"
	mkdir -p "$ADMIN"
	RAW_STATUS=$(grep -E '(^|/)var/lib/dpkg/status$' "$LISTING_RAW" | head -1 || true)
	RAW_INFO=$(grep -E '(^|/)var/lib/dpkg/info/[^/]+\.list$' "$LISTING_RAW" || true)
	if [[ -n $RAW_STATUS ]]; then
		# shellcheck disable=SC2086
		tar -xf "$SRC_PATH" -C "$ADMIN" --no-recursion $RAW_STATUS $RAW_INFO 2>/dev/null || true
	fi
	STATUS="$ADMIN/${SRC_PREFIX:+$SRC_PREFIX/}var/lib/dpkg/status"
	ADMIN_ROOT="$ADMIN/${SRC_PREFIX:+$SRC_PREFIX/}var/lib/dpkg"

	if [[ $WITH_DEPS -eq 1 && -f $STATUS ]]; then
		log "依赖闭包   : 从 $(grep -c '^Package:' "$STATUS" 2>/dev/null || echo 0) 个包里算"
		# shellcheck disable=SC2086
		PKGS=$(resolve_closure "$STATUS" $BASE_PACKAGES)
		MODE="packages"
	else
		warn "tarball 里没有 dpkg status，退回按目录裁的模式（会比较粗）"
		MODE="paths"
	fi

	# ---- 生成要保留的成员清单 ----
	if [[ $MODE == packages ]]; then
		# shellcheck disable=SC2086
		files_for_packages "$ADMIN_ROOT" "${SRC_PREFIX:+$SRC_PREFIX}" $PKGS \
			| sed 's,^/,,' | sort -u > "$WORK/want.txt"
		# dpkg 的管理目录本身不属于任何包，得手动留着
		{
			printf '%s\n' \
				"${SRC_PREFIX:+$SRC_PREFIX/}var/lib/dpkg/status" \
				"${SRC_PREFIX:+$SRC_PREFIX/}var/lib/dpkg/available" \
				"${SRC_PREFIX:+$SRC_PREFIX/}var/lib/dpkg/diversions"
			sed "s,^,${SRC_PREFIX:+$SRC_PREFIX/}," < <(printf '%s\n' var/lib/dpkg/info var/lib/dpkg/updates var/lib/dpkg/parts var/lib/dpkg/triggers var/lib/dpkg/alternatives var/cache/apt/archives/partial var/log var/run tmp)
			# info/*.list、triggers/* 这些是 dpkg 的管理数据，不归任何包所有
			grep -E "^(${SRC_PREFIX:+$SRC_PREFIX/})var/lib/dpkg/(info|triggers|alternatives|updates|parts)/" "$LISTING" || true
		} >> "$WORK/want.txt"
		# dpkg 自己的数据（triplets 等）apt 的方法，按目录兜底
		grep -E "^(${SRC_PREFIX:+$SRC_PREFIX/})(usr/share/dpkg|usr/lib/apt|etc/apt|etc/dpkg)/" "$LISTING" \
			>> "$WORK/want.txt" || true
	else
		cp "$LISTING" "$WORK/want.txt"
	fi
	# --keep-path 追加：即使不属于保留的包，也硬留下这些路径
	if [[ -n $KEEP_PATHS ]]; then
		for p in $KEEP_PATHS; do
			grep -E "^(${SRC_PREFIX:+$SRC_PREFIX/})${p#/}(/|$)" "$LISTING" >> "$WORK/want.txt" || true
		done
	fi
	sort -u -o "$WORK/want.txt" "$WORK/want.txt"

	# 只留 tarball 里真有的成员，再套裁剪规则
	awk 'NR==FNR { have[$0]=1; next } ($0 in have) { print }' \
		"$LISTING" "$WORK/want.txt" > "$WORK/exist.txt"

	FILELIST="$WORK/keep.txt"
	while IFS= read -r m; do
		is_dropped "$m" && continue
		printf '%s\n' "$m"
	done < "$WORK/exist.txt" > "$FILELIST"

	KEPT=$(wc -l < "$FILELIST")
	TOTAL=$(wc -l < "$LISTING")
	log "保留文件   : $KEPT / $TOTAL"
	if [[ -n $PKGS ]]; then
		log "保留包     : $(printf '%s\n' "$PKGS" | tr '\n' ' ')"
	fi
	if [[ $MODE == packages ]] && [[ $KEPT -lt 5 ]]; then
		die "只保留到 $KEPT 个文件，明显不对（.list 缺失？用 --no-deps 试试）"
	fi
else
	log "保留规则   : 基础包 [$BASE_PACKAGES] + Depends 闭包"
	log "裁剪规则   : 丢掉 ${DROP_PATHS[*]}"
	log "（源 tarball 不在本地，dry-run 不下载，所以算不出文件数）"
fi

# 重打包时的路径改写表达式。tar 成员必须是相对路径（解到 / 才落对地方），
# 所以去掉 TARGET_PREFIX 的前导斜杠。
REL_PREFIX=${TARGET_PREFIX#/}
if [[ -n $SRC_PREFIX ]]; then
	TRANSFORM="s,^\\./$SRC_PREFIX,$REL_PREFIX,"
else
	TRANSFORM="s,^\\./,$REL_PREFIX/,"
fi

log "tar 命令   : tar --zstd -cf $OUT_TAR -C <staging> --transform '$TRANSFORM' ."
log "设备上解包 : tar -xf $(basename "$OUT_TAR") -C /   # → /var/k60bootstrap"

if [[ $DRY_RUN -eq 1 ]]; then
	log "dry-run：到此为止，没有下载、没有写任何文件"
	exit 0
fi

# ---- 真干活 ----
[[ $KEPT -gt 0 ]] || die "一个文件都没保留，规则有问题"
mkdir -p "$OUT_DIR"
STAGE="$WORK/stage"
mkdir -p "$STAGE"

log "解出保留的文件到 staging..."
# FILELIST 里是规范化名字；tar 要的是 tarball 里原样的成员名（可能带 ./）
awk 'NR==FNR { keep[$0]=1; next } { n=$0; sub(/^\.\//,"",n); if (n in keep) print }' \
	"$FILELIST" "$LISTING_RAW" > "$WORK/keep.raw"
tar -xf "$SRC_PATH" -C "$STAGE" --no-recursion -T "$WORK/keep.raw" || die "解包失败"

# 空目录：tar 从文件清单里造不出来，手动补（dpkg/apt 要它们存在）
for d in var/lib/dpkg/updates var/lib/dpkg/parts var/lib/dpkg/triggers \
	var/lib/dpkg/alternatives var/lib/dpkg/info var/cache/apt/archives/partial \
	var/log var/run tmp usr/lib/apt/methods; do
	mkdir -p "$STAGE/${SRC_PREFIX:+$SRC_PREFIX/}$d"
done

log "重打包..."
rm -f "$OUT_TAR"
tar --zstd --owner=0 --group=0 --numeric-owner -cf "$OUT_TAR" \
	-C "$STAGE" --transform "$TRANSFORM" . || die "打包失败"

# ---- 附属文件 ----
cat > "$OUT_DIR/k60bootstrap.env" <<EOF
# 由 tools/bootstrap/build_bootstrap.sh 生成（$(date -Iseconds)）
# 设备上: . /path/to/k60bootstrap.env
K60_BOOTSTRAP=$TARGET_PREFIX
export K60_BOOTSTRAP
export PATH="$TARGET_PREFIX/usr/bin:$TARGET_PREFIX/bin:\$PATH"
export DPKG_ADMINDIR="$TARGET_PREFIX/var/lib/dpkg"
export APT_CONFIG="$TARGET_PREFIX/etc/apt/apt.conf"
export LD_LIBRARY_PATH="$TARGET_PREFIX/usr/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
EOF

{
	printf '# k60bootstrap %s\n' "$STAMP"
	printf 'source   : %s\n' "$SRC_PATH"
	printf 'src prefix: %s\n' "${SRC_PREFIX:-<rootful>}"
	printf 'target   : %s\n' "$TARGET_PREFIX"
	printf 'mode     : %s\n' "${MODE:-unknown}"
	printf 'files    : %s / %s\n' "${KEPT:-?}" "${TOTAL:-?}"
	printf 'size     : %s\n' "$(du -h "$OUT_TAR" | cut -f1)"
	printf 'sha256   : %s\n' "$(sha256sum "$OUT_TAR" | cut -d' ' -f1)"
	printf 'packages :\n'
	if [[ -n $PKGS ]]; then printf '%s\n' "$PKGS" | sed 's/^/  /'; else echo "  (按目录裁，未按包)"; fi
	printf 'dropped  : %s\n' "${DROP_PATHS[*]}"
} > "$OUT_DIR/manifest.txt"

log "完成: $OUT_TAR ($(du -h "$OUT_TAR" | cut -f1))"
log "清单: $OUT_DIR/manifest.txt"
log "上机: tar -xf $(basename "$OUT_TAR") -C / && . $OUT_DIR/k60bootstrap.env"
log "      $TARGET_PREFIX/usr/bin/dpkg --version"
