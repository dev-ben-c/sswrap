#!/bin/sh
# Build/refresh an HD texture pack from sswrap's texture dump with Real-ESRGAN.
# Only textures that are not in the load folder yet are processed, so re-running after more
# play (with TextureDump=1) just adds the new ones.
#
#   GAME_DIR=/path/to/Starsiege REALESRGAN=/path/to/realesrgan-ncnn-vulkan ./build.sh
#
# REALESRGAN: https://github.com/xinntao/Real-ESRGAN/releases (realesrgan-ncnn-vulkan,
# runs on any Vulkan GPU). MODEL defaults to realesrgan-x4plus; SCALE to 4.
set -e
here=$(cd "$(dirname "$0")" && pwd)
: "${GAME_DIR:?set GAME_DIR to the Starsiege folder}"
: "${REALESRGAN:?set REALESRGAN to the realesrgan-ncnn-vulkan binary}"
MODEL=${MODEL:-realesrgan-x4plus}; SCALE=${SCALE:-4}
dump="$GAME_DIR/sswrap_textures/dump"; load="$GAME_DIR/sswrap_textures/load"
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
mkdir -p "$work/in" "$work/out" "$load"

python3 "$here/classify.py" "$dump" "$work/classes.json"
python3 - "$work/classes.json" "$load" "$work/todo.txt" <<'PY'
import json, os, sys
good = json.load(open(sys.argv[1]))['good']
have = {f.rsplit('.', 1)[0] for f in os.listdir(sys.argv[2])}
todo = [f for f in good if f[:-4] not in have]
open(sys.argv[3], 'w').write('\n'.join(todo))
print(f"{len(good)} usable textures, {len(todo)} new to upscale")
PY
[ -s "$work/todo.txt" ] || { echo "nothing new"; exit 0; }
python3 "$here/prep.py" "$dump" "$work/todo.txt" "$work/in"
"$REALESRGAN" -i "$work/in" -o "$work/out" -n "$MODEL" -s "$SCALE" -f png -m "$(dirname "$REALESRGAN")/models"
python3 "$here/post.py" "$work/out" "$load" "$SCALE"
echo "load folder now has $(ls "$load" | wc -l) textures"
