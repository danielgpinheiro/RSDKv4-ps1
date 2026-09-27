#!/bin/bash
# Host check: PS1SortDrawList3D (RSDKv4/Scene3D.cpp) == upstream's bubble sort (docs/30 phase 2.3; from the Sonic CD port).
set -eu
cd "$(dirname "$0")/../.."
T=$(mktemp -d)
python3 - "$T/extract.inc" <<'PY'
import sys
s = open('RSDKv4/Scene3D.cpp').read()
i = s.index('void PS1SortDrawList3DRange')
j = s.index('void PS1SortDrawList3D(', i)
open(sys.argv[1], 'w').write(s[i:s.index('\n}\n', j) + 3])
PY
c++ -std=c++17 -O2 -DSORT3D_EXTRACT="\"$T/extract.inc\"" tools/scene3d/sort3d_check.cpp -o "$T/sort3d"
"$T/sort3d"
rm -rf "$T"
