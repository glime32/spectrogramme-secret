#!/usr/bin/env bash
# Cache des damiers dans un son, les fait réapparaître et vérifie chaque quart de l'image.
cd "$(dirname "$0")"
make -s spectro || exit 1
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
ok=0 total=0
resultat() { # description, commande...
  local desc="$1"; shift
  total=$((total + 1))
  if "$@" > /dev/null 2>&1; then ok=$((ok + 1)); echo "  ok    $desc"; else echo "  ÉCHEC $desc"; fi
}

damier() { # largeur hauteur inverse(0/1) -> PGM texte : haut-gauche et bas-droite blancs
  echo "P2 $1 $2 255"
  for ((y = 0; y < $2; y++)); do
    for ((x = 0; x < $1; x++)); do
      (( ((x < $1 / 2) == (y < $2 / 2)) != $3 )) && printf '255 ' || printf '0 '
    done
    echo
  done
}

quarts() { # image.pgm (P5) -> luminosité moyenne de chaque quart : HG HD BG BD
  local w h debut
  read -r w h < <(sed -n 2p "$1")
  debut=$(( $(head -n 3 "$1" | wc -c) + 1 ))
  tail -c +"$debut" "$1" | od -An -v -tu1 | awk -v w="$w" -v h="$h" '
    { for (i = 1; i <= NF; i++) { q = (int(n / w) >= h / 2) * 2 + (n % w >= w / 2); s[q] += $i; c[q]++; n++ } }
    END { printf "%d %d %d %d\n", s[0] / c[0], s[1] / c[1], s[2] / c[2], s[3] / c[3] }'
}

ressemble() { # image.pgm, motif "1001" (1 = quart clair) : clair > 40, sombre < 15
  local moy motif=$2 i=0
  read -ra moy < <(quarts "$1")
  for v in "${moy[@]}"; do
    if [ "${motif:i:1}" = 1 ]; then ((v > 40)) || return 1; else ((v < 15)) || return 1; fi
    i=$((i + 1))
  done
}

wav_coherent() { # en-tête RIFF/WAVE et taille annoncée = taille réelle
  [ "$(head -c 4 "$1")" = RIFF ] && [ "$(tail -c +9 "$1" | head -c 8)" = "WAVEfmt " ] || return 1
  local annonce
  annonce=$(tail -c +41 "$1" | head -c 4 | od -An -tu4 | tr -d ' ')
  [ $((annonce + 44)) = "$(stat -c %s "$1")" ]
}

damier 40 20 0 > "$tmp/damier.pgm"
damier 40 20 1 > "$tmp/inverse.pgm"

./spectro cacher -i "$tmp/damier.pgm" -o "$tmp/seul.wav" 2> /dev/null
./spectro reveler -i "$tmp/seul.wav" -o "$tmp/seul.pgm" --fmin 500 --fmax 8000 2> /dev/null
resultat "le WAV écrit est cohérent"                      wav_coherent "$tmp/seul.wav"
resultat "damier caché puis révélé : les 4 quarts"         ressemble "$tmp/seul.pgm" 1001

./spectro cacher -i "$tmp/inverse.pgm" --dans "$tmp/seul.wav" -o "$tmp/melange.wav" 2> /dev/null
./spectro reveler -i "$tmp/melange.wav" -o "$tmp/aigus.pgm" --fmin 15000 --fmax 19500 2> /dev/null
./spectro reveler -i "$tmp/melange.wav" -o "$tmp/graves.pgm" --fmin 500 --fmax 8000 2> /dev/null
resultat "--dans : le damier inversé apparaît dans les aigus" ressemble "$tmp/aigus.pgm" 0110
resultat "--dans : le son d'origine est intact en dessous"  ressemble "$tmp/graves.pgm" 1001

./spectro cacher -t "SALUT ♥" -o "$tmp/texte.wav" 2> /dev/null
resultat "un texte avec accents et ♥ donne un WAV cohérent" wav_coherent "$tmp/texte.wav"

if command -v magick > /dev/null; then
  ./spectro reveler -i "$tmp/seul.wav" -o "$tmp/seul.png" --fmin 500 --fmax 8000 2> /dev/null
  magick "$tmp/seul.png" "$tmp/relu.pgm" 2> /dev/null
  resultat "le PNG fait main se relit pixel pour pixel (magick)" cmp <(tail -n +4 "$tmp/seul.pgm") <(tail -n +4 "$tmp/relu.pgm")
fi

resultat "fmin ≥ fmax : refusé"            sh -c "! ./spectro cacher -t A -o '$tmp/x.wav' --fmin 9000 --fmax 8000"
resultat "--fft qui n'est pas 2^k : refusé" sh -c "! ./spectro reveler -i '$tmp/seul.wav' -o '$tmp/x.pgm' --fft 1000"

echo "$ok / $total"
[ "$ok" = "$total" ]
