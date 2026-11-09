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

refuse() { "$@" > /dev/null 2>&1; [ $? = 1 ]; } # message d'erreur, pas de plantage
cacher_pgm() { printf "$1" > "$tmp/abime.pgm"; ./spectro cacher -i "$tmp/abime.pgm" -o "$tmp/x.wav"; }
reveler_wav() { ./spectro reveler -i "$1" -o "$tmp/x.pgm"; }
remplace() { # fichier position octets(printf) -> abime.wav, 4 octets remplacés
  { head -c "$2" "$1"; printf "$3"; tail -c +$(($2 + 5)) "$1"; } > "$tmp/abime.wav"
}

resultat "PGM plus court que sa taille annoncée : refusé"  refuse cacher_pgm 'P5 40 20 255\n\377\377\377\377'
resultat "PGM de 99999999999 pixels de large : refusé"     refuse cacher_pgm 'P5 99999999999 2 255\n\377\377'
resultat "PGM avec un pixel au-dessus du maximum : refusé" refuse cacher_pgm 'P2 2 1 10\n5 11\n'
resultat "PGM 16 bits coupé au milieu d'un pixel : refusé" refuse cacher_pgm 'P5 1 1 65535\n\377'
printf 'RIFF\044\0\0\0WAVEfmt \004\0\0\0\001\0\001\0data\0\0\0\0' > "$tmp/abime.wav"
resultat "WAV dont le bloc fmt est trop court : refusé"    refuse reveler_wav "$tmp/abime.wav"
remplace "$tmp/seul.wav" 24 '\0\0\0\0'
resultat "WAV à 0 Hz : refusé"                             refuse reveler_wav "$tmp/abime.wav"
head -c 44 "$tmp/seul.wav" > "$tmp/vide.wav"
resultat "WAV sans aucun échantillon : refusé"             refuse reveler_wav "$tmp/vide.wav"
remplace "$tmp/seul.wav" 40 '\377\377\377\377'
./spectro reveler -i "$tmp/abime.wav" -o "$tmp/direct.pgm" --fmin 500 --fmax 8000 2> /dev/null
resultat "WAV de taille 0xFFFFFFFF (en direct) : lu en entier" cmp "$tmp/direct.pgm" "$tmp/seul.pgm"

echo "$ok / $total"
[ "$ok" = "$total" ]
