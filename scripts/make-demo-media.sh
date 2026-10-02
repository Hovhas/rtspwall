#!/bin/sh
# make-demo-media.sh — renders the README media in docs/media/ from the
# bundled demo clips (demo/cam1.mp4 .. cam5.mp4, CC0).
#
# Usage: sh scripts/make-demo-media.sh        (run from anywhere; needs ffmpeg)
#
# Output:
#   docs/media/demo.gif             ~10 s looping 2x2 wall, 638 px, <= 3 MB
#   docs/media/demo.mp4             the same at 798 px as H.264 (smaller)
#   docs/media/social-preview.png   1280x640 GitHub social preview
#
# These are RENDERED ILLUSTRATIONS of what the wall shows with
# demo/demo.conf — composed here with ffmpeg xstack, not screen captures of
# a Pi — and every image says so in a corner label. The layout follows
# demo/demo.conf: cells 1-3 fixed, cell 4 alternates between cam4 and cam5.
# The real rotation period (ROTATE_SECONDS, 8 s) is shortened so that both
# clips appear within one 10 s loop; like on the Pi, both clips keep
# playing, so the switch has no black frame.
set -eu

cd "$(dirname "$0")/.."

command -v ffmpeg >/dev/null 2>&1 || {
	echo "make-demo-media.sh: ffmpeg not found (sudo apt install ffmpeg)" >&2
	exit 1
}

conf=demo/demo.conf
grid=$(sed -n 's/^GRID=//p' "$conf")
rotate=$(sed -n 's/^ROTATE_SECONDS=//p' "$conf")
if [ "$grid" != "2x2" ]; then
	echo "make-demo-media.sh: expected GRID=2x2 in $conf, got '$grid'" >&2
	exit 1
fi

[ -f demo/cam5.mp4 ] || make demo-clips

duration=10                     # length of the demo clips (make-clips.sh)
switch=$((duration / 2))        # cell 4: cam4 first, cam5 from here on
if [ "${rotate:-0}" -gt 0 ] && [ "$rotate" -lt "$switch" ]; then
	switch=$rotate
fi

# Wall: four 16:9 cells with a 2 px seam on a dark background, as on a
# real composition.
gap=2
bg=0x101010

font=font=Sans
bold=font=Sans:style=Bold
dejavu=/usr/share/fonts/truetype/dejavu
if [ -f "$dejavu/DejaVuSans.ttf" ]; then
	font=fontfile=$dejavu/DejaVuSans.ttf
fi
if [ -f "$dejavu/DejaVuSans-Bold.ttf" ]; then
	bold=fontfile=$dejavu/DejaVuSans-Bold.ttf
fi

out=docs/media
mkdir -p "$out"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

# Text goes through textfile= so commas, colons and dashes need no
# filtergraph escaping.
printf '%s' 'rendered illustration from the bundled demo clips' >"$work/label.txt"
printf '%s' 'rtspwall' >"$work/title.txt"
printf '%s\n%s\n%s\n%s' \
	'A 24/7 RTSP camera wall' \
	'for the Raspberry Pi 4 —' \
	'hardware-decoded,' \
	'no desktop' >"$work/subtitle.txt"

ff() {
	ffmpeg -nostdin -hide_banner -loglevel error -y "$@"
}

# render_wall CELL_W CELL_H LABEL_PX OUTFILE — the 2x2 wall, full frame
# rate, near-lossless. Rendered at the target size (not scaled afterwards)
# so the corner label stays sharp.
render_wall() {
	cw=$1
	ch=$2
	x2=$((cw + gap))
	y2=$((ch + gap))
	ff -i demo/cam1.mp4 -i demo/cam2.mp4 -i demo/cam3.mp4 \
		-i demo/cam4.mp4 -i demo/cam5.mp4 \
		-filter_complex "\
[0:v]scale=$cw:${ch}[c1];[1:v]scale=$cw:${ch}[c2];[2:v]scale=$cw:${ch}[c3];\
[3:v]scale=$cw:${ch}[c4];[4:v]scale=$cw:${ch}[c5];\
[c4][c5]overlay=enable='gte(t,$switch)'[r4];\
[c1][c2][c3][r4]xstack=inputs=4:layout=0_0|${x2}_0|0_${y2}|${x2}_${y2}:fill=$bg,\
drawtext=$font:textfile=$work/label.txt:fontsize=$3:fontcolor=white@0.85:\
box=1:boxcolor=black@0.6:boxborderw=5:x=w-tw-10:y=h-th-10[v]" \
		-map "[v]" -t "$duration" -an -c:v libx264 -crf 10 -preset fast \
		-pix_fmt yuv420p "$4"
}

# 1. The wall at 798x450 (MP4, social preview) and 638x358 (GIF).
render_wall 398 224 13 "$work/wall.mp4"
render_wall 318 178 12 "$work/wall-small.mp4"

# 2. GIF: 12 fps, one palette for the whole clip, loops forever.
ff -i "$work/wall-small.mp4" \
	-vf "fps=12,palettegen=max_colors=128:stats_mode=diff" "$work/palette.png"
ff -i "$work/wall-small.mp4" -i "$work/palette.png" -filter_complex \
	"[0:v]fps=12[f];[f][1:v]paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle" \
	-loop 0 "$out/demo.gif"

# 3. MP4 alternative (plays inline on GitHub, much smaller than the GIF).
ff -i "$work/wall.mp4" -an -c:v libx264 -profile:v high -crf 28 -preset slow \
	-pix_fmt yuv420p -movflags +faststart \
	-metadata title="rtspwall demo wall (rendered illustration)" \
	"$out/demo.mp4"

# 4. Social preview: title and tagline left, a still of the wall right.
pw=688
ph=388
px=$((1280 - pw - 48))
py=$(((640 - ph) / 2))
ff -ss 3 -i "$work/wall.mp4" -frames:v 1 "$work/still.png"
ff -f lavfi -i "color=c=0x0d1117:s=1280x640:d=1" -i "$work/still.png" \
	-filter_complex "\
[1:v]scale=$pw:${ph}[w];[0:v][w]overlay=$px:$py,\
drawbox=x=56:y=262:w=72:h=4:color=0x3fb950:t=fill,\
drawtext=$bold:textfile=$work/title.txt:fontsize=80:fontcolor=white:x=52:y=150,\
drawtext=$font:textfile=$work/subtitle.txt:fontsize=30:line_spacing=14:\
fontcolor=0xc9d1d9:x=56:y=296" \
	-frames:v 1 "$out/social-preview.png"

ls -l "$out"
