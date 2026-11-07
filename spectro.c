/*
 * spectro — cacher une image (ou un texte) dans le spectrogramme d'un son,
 * puis la faire réapparaître.
 *
 *   cacher  : synthèse additive, une sinusoïde par ligne de l'image,
 *             amplitude = luminosité du pixel (échelle en dB).
 *   reveler : transformée de Fourier à court terme (FFT écrite à la main,
 *             fenêtre de Hann), puis magnitude en dB -> niveaux de gris.
 *
 * Aucune dépendance à part la libm. Lit/écrit du WAV, du PGM/PPM, écrit du PNG.
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void meurt(const char *msg, const char *detail) {
    fprintf(stderr, "spectro : %s%s%s\n", msg, detail ? " : " : "", detail ? detail : "");
    exit(1);
}

static void *alloue(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) meurt("plus de mémoire", NULL);
    return p;
}

/* ------------------------------------------------------------------ image */

typedef struct {
    int w, h;
    float *px; /* luminosité 0..1, ligne par ligne, (0,0) en haut à gauche */
} Image;

static Image image_nouvelle(int w, int h) {
    Image im = {w, h, alloue(sizeof(float) * (size_t)w * h)};
    return im;
}

static int lit_entier_pnm(FILE *f) {
    int c, v = 0;
    do { /* sauter blancs et commentaires */
        c = fgetc(f);
        if (c == '#') while (c != '\n' && c != EOF) c = fgetc(f);
    } while (c == ' ' || c == '\t' || c == '\n' || c == '\r');
    if (c < '0' || c > '9') meurt("fichier PNM mal formé", NULL);
    while (c >= '0' && c <= '9') { v = v * 10 + (c - '0'); c = fgetc(f); }
    return v;
}

/* PGM (P2, P5) ou PPM (P3, P6) -> niveaux de gris */
static Image image_lit_pnm(const char *chemin) {
    FILE *f = fopen(chemin, "rb");
    if (!f) meurt("impossible d'ouvrir l'image", chemin);
    char m[3] = {0};
    if (fread(m, 1, 2, f) != 2 || m[0] != 'P' || !strchr("2356", m[1]))
        meurt("format d'image non reconnu (PGM ou PPM attendu ; convertis avec : magick photo.png photo.pgm)", chemin);
    int w = lit_entier_pnm(f), h = lit_entier_pnm(f), maxv = lit_entier_pnm(f);
    if (w <= 0 || h <= 0 || maxv <= 0 || maxv > 65535) meurt("dimensions d'image invalides", chemin);
    int canaux = (m[1] == '3' || m[1] == '6') ? 3 : 1;
    int binaire = (m[1] == '5' || m[1] == '6');
    Image im = image_nouvelle(w, h);
    for (long i = 0; i < (long)w * h; i++) {
        double s = 0;
        for (int c = 0; c < canaux; c++) {
            int v;
            if (!binaire) v = lit_entier_pnm(f);
            else if (maxv < 256) v = fgetc(f);
            else { int a = fgetc(f); v = (a << 8) | fgetc(f); }
            if (v < 0) meurt("image tronquée", chemin);
            /* luminance perçue (Rec. 601) */
            s += (canaux == 3 ? (c == 0 ? 0.299 : c == 1 ? 0.587 : 0.114) : 1.0) * v;
        }
        im.px[i] = (float)(s / maxv);
    }
    fclose(f);
    return im;
}

/* --- PNG minimal : blocs deflate « stockés » (sans compression), CRC32, Adler-32 */

static uint32_t crc_table[256];
static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
static uint32_t crc_maj(uint32_t crc, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) crc = crc_table[(crc ^ b[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}
static void ecrit_be32(FILE *f, uint32_t v) {
    uint8_t b[4] = {v >> 24, v >> 16, v >> 8, v};
    fwrite(b, 1, 4, f);
}
static void png_chunk(FILE *f, const char *type, const uint8_t *data, size_t n) {
    ecrit_be32(f, (uint32_t)n);
    uint32_t crc = crc_maj(0xFFFFFFFFu, (const uint8_t *)type, 4);
    crc = crc_maj(crc, data, n);
    fwrite(type, 1, 4, f);
    if (n) fwrite(data, 1, n, f);
    ecrit_be32(f, crc ^ 0xFFFFFFFFu);
}
static void png_ecrit(const char *chemin, int w, int h, const uint8_t *gris) {
    crc_init();
    size_t ligne = (size_t)w + 1, brut_n = ligne * h;
    uint8_t *brut = alloue(brut_n);
    for (int y = 0; y < h; y++) {
        brut[y * ligne] = 0; /* filtre « None » */
        memcpy(brut + y * ligne + 1, gris + (size_t)y * w, w);
    }
    size_t nblocs = brut_n / 65535 + 1;
    size_t z_n = 2 + brut_n + nblocs * 5 + 4;
    uint8_t *z = alloue(z_n), *p = z;
    *p++ = 0x78; *p++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t pos = 0; pos < brut_n || pos == 0;) {
        size_t n = brut_n - pos > 65535 ? 65535 : brut_n - pos;
        *p++ = (pos + n == brut_n) ? 1 : 0;
        *p++ = n & 0xFF; *p++ = n >> 8; *p++ = ~n & 0xFF; *p++ = (~n >> 8) & 0xFF;
        memcpy(p, brut + pos, n);
        for (size_t i = 0; i < n; i++) { a = (a + brut[pos + i]) % 65521; b = (b + a) % 65521; }
        p += n; pos += n;
        if (n == 0) break;
    }
    uint32_t adler = (b << 16) | a;
    *p++ = adler >> 24; *p++ = adler >> 16; *p++ = adler >> 8; *p++ = adler;
    FILE *f = fopen(chemin, "wb");
    if (!f) meurt("impossible d'écrire", chemin);
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13] = {w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 0, 0, 0, 0};
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, (size_t)(p - z));
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(brut); free(z);
}

static int finit_par(const char *s, const char *suffixe) {
    size_t a = strlen(s), b = strlen(suffixe);
    return a >= b && strcasecmp(s + a - b, suffixe) == 0;
}

static void image_ecrit(const char *chemin, const Image *im) {
    uint8_t *g = alloue((size_t)im->w * im->h);
    for (long i = 0; i < (long)im->w * im->h; i++) {
        float v = im->px[i] < 0 ? 0 : im->px[i] > 1 ? 1 : im->px[i];
        g[i] = (uint8_t)lrintf(v * 255);
    }
    if (finit_par(chemin, ".png")) png_ecrit(chemin, im->w, im->h, g);
    else {
        FILE *f = fopen(chemin, "wb");
        if (!f) meurt("impossible d'écrire", chemin);
        fprintf(f, "P5\n%d %d\n255\n", im->w, im->h);
        fwrite(g, 1, (size_t)im->w * im->h, f);
        fclose(f);
    }
    free(g);
}

/* ------------------------------------------------------- texte -> image */

/* police 5x7 : 5 colonnes, bit 0 = ligne du haut */
static const char *POLICE_CAR = " !'(),-.0123456789:?ABCDEFGHIJKLMNOPQRSTUVWXYZ*";
static const uint8_t POLICE[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x00, 0x07, 0x00, 0x00},
    {0x00, 0x1C, 0x22, 0x41, 0x00}, {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x00, 0x50, 0x30, 0x00, 0x00},
    {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00},
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00}, {0x42, 0x61, 0x51, 0x49, 0x46},
    {0x21, 0x41, 0x45, 0x4B, 0x31}, {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03}, {0x36, 0x49, 0x49, 0x49, 0x36},
    {0x06, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x36, 0x36, 0x00, 0x00}, {0x02, 0x01, 0x51, 0x09, 0x06},
    {0x7C, 0x12, 0x11, 0x12, 0x7C}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F},
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}, {0x0C, 0x1E, 0x3C, 0x1E, 0x0C},
};

/* décode un caractère UTF-8 et le ramène à la police (accents retirés, ♥ -> *) */
static char car_police(const unsigned char **s) {
    unsigned c = *(*s)++;
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? (char)(c - 32) : (char)c;
    unsigned cp = 0, suite = 0;
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; suite = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; suite = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; suite = 3; }
    while (suite-- && (**s & 0xC0) == 0x80) cp = (cp << 6) | (*(*s)++ & 0x3F);
    if (cp == 0x2665 || cp == 0x2764) return '*';
    const char *de = "àâäÀÂÄçÇéèêëÉÈÊËîïÎÏôöÔÖùûüÙÛÜÿ", *vers = "AAAAAACCEEEEEEEEIIIIOOOOUUUUUUY";
    for (const unsigned char *p = (const unsigned char *)de; *p; vers++) {
        unsigned q = 0;
        q = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        p += 2;
        if (q == cp) return *vers;
    }
    return '?';
}

static Image image_texte(const char *texte, int echelle) {
    char buf[512];
    int n = 0;
    for (const unsigned char *s = (const unsigned char *)texte; *s && n < 511;) {
        char c = car_police(&s);
        if (c == '\n') c = ' ';
        buf[n++] = strchr(POLICE_CAR, c) ? c : '?';
    }
    if (n == 0) meurt("texte vide", NULL);
    int marge = 2;
    int w = (n * 6 - 1 + 2 * marge) * echelle, h = (7 + 2 * marge) * echelle;
    Image im = image_nouvelle(w, h);
    for (int i = 0; i < n; i++) {
        const uint8_t *g = POLICE[strchr(POLICE_CAR, buf[i]) - POLICE_CAR];
        for (int col = 0; col < 5; col++)
            for (int lig = 0; lig < 7; lig++)
                if (g[col] >> lig & 1)
                    for (int dy = 0; dy < echelle; dy++)
                        for (int dx = 0; dx < echelle; dx++) {
                            int x = (marge + i * 6 + col) * echelle + dx, y = (marge + lig) * echelle + dy;
                            im.px[(size_t)y * w + x] = 1.0f;
                        }
    }
    return im;
}

/* ------------------------------------------------------------------- WAV */

typedef struct {
    int taux, canaux;
    long n;      /* nombre de trames */
    float *ech;  /* entrelacé, -1..1 */
} Son;

static uint32_t le32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static uint16_t le16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }

static Son wav_lit(const char *chemin) {
    FILE *f = fopen(chemin, "rb");
    if (!f) meurt("impossible d'ouvrir le son", chemin);
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        meurt("ce n'est pas un WAV (convertis avec : ffmpeg -i son.mp3 son.wav)", chemin);
    int format = 0, canaux = 0, taux = 0, bits = 0;
    uint8_t *data = NULL;
    uint32_t data_n = 0;
    uint8_t ch[8];
    while (fread(ch, 1, 8, f) == 8) {
        uint32_t taille = le32(ch + 4);
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t fmt[64] = {0};
            size_t lu = taille < sizeof fmt ? taille : sizeof fmt;
            if (fread(fmt, 1, lu, f) != lu) meurt("en-tête WAV tronqué", chemin);
            if (taille > lu) fseek(f, taille - lu, SEEK_CUR);
            format = le16(fmt); canaux = le16(fmt + 2); taux = (int)le32(fmt + 4); bits = le16(fmt + 14);
            if (format == 0xFFFE && taille >= 26) format = le16(fmt + 24); /* WAVE_FORMAT_EXTENSIBLE */
        } else if (!memcmp(ch, "data", 4)) {
            data = alloue(taille);
            data_n = (uint32_t)fread(data, 1, taille, f); /* tolère un fichier coupé */
            break;
        } else fseek(f, taille + (taille & 1), SEEK_CUR);
        if (taille & 1 && memcmp(ch, "fmt ", 4) == 0) fseek(f, 1, SEEK_CUR);
    }
    fclose(f);
    if (!data || !canaux || !taux) meurt("WAV sans données", chemin);
    int octets = bits / 8;
    if (!((format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (format == 3 && bits == 32)))
        meurt("encodage WAV non géré (PCM 8/16/24/32 bits ou float 32 bits)", chemin);
    Son s = {taux, canaux, (long)(data_n / (octets * canaux)), NULL};
    s.ech = alloue(sizeof(float) * s.n * canaux);
    for (long i = 0; i < s.n * canaux; i++) {
        const uint8_t *b = data + i * octets;
        float v;
        if (format == 3) { float x; memcpy(&x, b, 4); v = x; }
        else if (bits == 8) v = (b[0] - 128) / 128.0f;
        else if (bits == 16) v = (int16_t)le16(b) / 32768.0f;
        else if (bits == 24) v = (int32_t)((uint32_t)b[0] << 8 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 24) / 2147483648.0f;
        else v = (int32_t)le32(b) / 2147483648.0f;
        s.ech[i] = v;
    }
    free(data);
    return s;
}

static void wav_ecrit(const char *chemin, const Son *s) {
    FILE *f = fopen(chemin, "wb");
    if (!f) meurt("impossible d'écrire", chemin);
    uint32_t data_n = (uint32_t)(s->n * s->canaux * 2);
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    uint32_t v = 36 + data_n;
    for (int i = 0; i < 4; i++) h[4 + i] = v >> (8 * i);
    memcpy(h + 8, "WAVEfmt ", 8);
    uint32_t champs[] = {16, 1 | (uint32_t)s->canaux << 16, (uint32_t)s->taux,
                         (uint32_t)s->taux * s->canaux * 2, (uint32_t)(s->canaux * 2) | 16u << 16};
    for (int k = 0; k < 5; k++)
        for (int i = 0; i < 4; i++) h[16 + 4 * k + i] = champs[k] >> (8 * i);
    memcpy(h + 36, "data", 4);
    for (int i = 0; i < 4; i++) h[40 + i] = data_n >> (8 * i);
    fwrite(h, 1, 44, f);
    for (long i = 0; i < s->n * s->canaux; i++) {
        float x = s->ech[i] > 1 ? 1 : s->ech[i] < -1 ? -1 : s->ech[i];
        int16_t q = (int16_t)lrintf(x * 32767);
        uint8_t b[2] = {(uint8_t)(q & 0xFF), (uint8_t)((uint16_t)q >> 8)};
        fwrite(b, 1, 2, f);
    }
    fclose(f);
}

/* ---------------------------------------------------------------- cacher */

/*
 * Une sinusoïde par ligne d'image. La ligne du haut est la fréquence fmax.
 * La phase de chaque ligne avance par un « phaseur » (rotation complexe),
 * ce qui évite un sin() par échantillon et garde la phase continue.
 * L'amplitude d'un pixel suit une échelle en décibels, comme un spectrogramme.
 */
static float *synthetise(const Image *im, int taux, long n, double fmin, double fmax, double dyn_db) {
    float *sortie = alloue(sizeof(float) * n);
    double *amp = alloue(sizeof(double) * im->w * im->h);
    for (long i = 0; i < (long)im->w * im->h; i++)
        amp[i] = im->px[i] < 0.02f ? 0 : pow(10, (im->px[i] - 1) * dyn_db / 20);
    uint32_t graine = 12345;
    for (int y = 0; y < im->h; y++) {
        double f = im->h == 1 ? fmax : fmax - (fmax - fmin) * y / (im->h - 1);
        double pas = 2 * M_PI * f / taux;
        double rc = cos(pas), rs = sin(pas);
        graine = graine * 1103515245u + 12345u; /* phase de départ aléatoire : évite un pic au début */
        double phi = 2 * M_PI * (graine >> 8) / 16777216.0;
        double c = cos(phi), s = sin(phi);
        const double *lig = amp + (size_t)y * im->w;
        int vide = 1;
        for (int x = 0; x < im->w; x++) if (lig[x] > 0) { vide = 0; break; }
        if (vide) continue;
        for (long t = 0; t < n; t++) {
            /* position dans l'image, interpolation linéaire entre colonnes */
            double u = (double)t / n * im->w - 0.5;
            int x0 = (int)floor(u);
            double fr = u - x0;
            double a0 = lig[x0 < 0 ? 0 : x0 >= im->w ? im->w - 1 : x0];
            double a1 = lig[x0 + 1 < 0 ? 0 : x0 + 1 >= im->w ? im->w - 1 : x0 + 1];
            double a = a0 + (a1 - a0) * fr;
            if (a > 0) sortie[t] += (float)(a * s);
            double c2 = c * rc - s * rs;
            s = s * rc + c * rs;
            c = c2;
            if ((t & 1023) == 0) { double r = 1 / sqrt(c * c + s * s); c *= r; s *= r; }
        }
    }
    free(amp);
    /* fondu de 10 ms aux extrémités, puis normalisation à 1 */
    long fondu = taux / 100;
    for (long t = 0; t < fondu && t < n / 2; t++) {
        float g = (float)t / fondu;
        sortie[t] *= g;
        sortie[n - 1 - t] *= g;
    }
    float pic = 1e-9f;
    for (long t = 0; t < n; t++) if (fabsf(sortie[t]) > pic) pic = fabsf(sortie[t]);
    for (long t = 0; t < n; t++) sortie[t] /= pic;
    return sortie;
}

/* ------------------------------------------------------------------- FFT */

/* FFT radix 2 itérative, en place. cs/sn : table des cos/sin pour n. */
static void fft(float *re, float *im, int n, const double *cs, const double *sn) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int demi = len / 2, pas = n / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < demi; k++) {
                double wr = cs[k * pas], wi = sn[k * pas];
                int a = i + k, b = a + demi;
                double vr = re[b] * wr - im[b] * wi, vi = re[b] * wi + im[b] * wr;
                re[b] = (float)(re[a] - vr); im[b] = (float)(im[a] - vi);
                re[a] = (float)(re[a] + vr); im[a] = (float)(im[a] + vi);
            }
    }
}

static Image spectrogramme(const Son *s, int nfft, int saut, double fmin, double fmax, double dyn_db) {
    long trames = s->n < nfft ? 1 : (s->n - nfft) / saut + 1;
    int kmin = (int)floor(fmin * nfft / s->taux), kmax = (int)ceil(fmax * nfft / s->taux);
    if (kmin < 0) kmin = 0;
    if (kmax > nfft / 2) kmax = nfft / 2;
    if (kmax <= kmin) meurt("bande de fréquences vide", NULL);
    Image im = image_nouvelle((int)trames, kmax - kmin + 1);
    double *cs = alloue(sizeof(double) * nfft), *sn = alloue(sizeof(double) * nfft), *hann = alloue(sizeof(double) * nfft);
    for (int i = 0; i < nfft; i++) {
        cs[i] = cos(-2 * M_PI * i / nfft); sn[i] = sin(-2 * M_PI * i / nfft);
        hann[i] = 0.5 - 0.5 * cos(2 * M_PI * i / (nfft - 1));
    }
    float *re = alloue(sizeof(float) * nfft), *ii = alloue(sizeof(float) * nfft);
    float maxdb = -1e30f;
    for (long tr = 0; tr < trames; tr++) {
        for (int i = 0; i < nfft; i++) {
            long t = tr * saut + i;
            float v = 0;
            if (t < s->n) for (int c = 0; c < s->canaux; c++) v += s->ech[t * s->canaux + c];
            re[i] = (float)(v / s->canaux * hann[i]);
            ii[i] = 0;
        }
        fft(re, ii, nfft, cs, sn);
        for (int k = kmin; k <= kmax; k++) {
            float db = 10 * log10f(re[k] * re[k] + ii[k] * ii[k] + 1e-20f);
            im.px[(size_t)(kmax - k) * im.w + tr] = db;
            if (db > maxdb) maxdb = db;
        }
    }
    for (long i = 0; i < (long)im.w * im.h; i++) im.px[i] = (float)((im.px[i] - (maxdb - dyn_db)) / dyn_db);
    free(cs); free(sn); free(hann); free(re); free(ii);
    return im;
}

/* -------------------------------------------------------------------- CLI */

static void aide(void) {
    puts("spectro — cacher une image dans le spectrogramme d'un son\n\n"
         "  spectro cacher  (-t \"TEXTE\" | -i image.pgm) -o sortie.wav [options]\n"
         "      --dans musique.wav   mélanger l'image dans un son existant\n"
         "      --fmin HZ --fmax HZ  bande utilisée (défaut 500..8000, ou 15000..19500 avec --dans)\n"
         "      --duree S            durée de l'image (défaut : 12 ms par colonne)\n"
         "      --debut S            début de l'image dans la musique (défaut 0)\n"
         "      --gain G             niveau de l'image mélangée, 0..1 (défaut 0.05)\n"
         "      --taille N           taille des lettres pour -t (défaut 8)\n"
         "      --taux HZ            fréquence d'échantillonnage sans --dans (défaut 44100)\n\n"
         "  spectro reveler -i son.wav -o image.png [options]\n"
         "      --fmin HZ --fmax HZ  bande à afficher (défaut : tout)\n"
         "      --fft N              taille de la FFT, puissance de 2 (défaut 2048)\n"
         "      --saut N             pas entre deux fenêtres (défaut 256)\n"
         "      --dyn DB             dynamique affichée (défaut 60)\n");
}

int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--aide")) { aide(); return argc < 2; }
    const char *mode = argv[1], *entree = NULL, *sortie = NULL, *texte = NULL, *dans = NULL;
    double fmin = -1, fmax = -1, duree = -1, debut = 0, gain = 0.05, dyn = 60;
    int taille = 8, taux = 44100, nfft = 2048, saut = 256;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define OPT(nom) (!strcmp(a, nom) && (v || (meurt("valeur manquante après", a), 0)) && ++i)
        if (OPT("-i")) entree = v;
        else if (OPT("-o")) sortie = v;
        else if (OPT("-t")) texte = v;
        else if (OPT("--dans")) dans = v;
        else if (OPT("--fmin")) fmin = atof(v);
        else if (OPT("--fmax")) fmax = atof(v);
        else if (OPT("--duree")) duree = atof(v);
        else if (OPT("--debut")) debut = atof(v);
        else if (OPT("--gain")) gain = atof(v);
        else if (OPT("--taille")) taille = atoi(v);
        else if (OPT("--taux")) taux = atoi(v);
        else if (OPT("--fft")) nfft = atoi(v);
        else if (OPT("--saut")) saut = atoi(v);
        else if (OPT("--dyn")) dyn = atof(v);
        else meurt("option inconnue", a);
    }
    if (!sortie) meurt("il manque -o sortie", NULL);

    if (!strcmp(mode, "cacher")) {
        if (!texte == !entree) meurt("donne soit -t \"TEXTE\", soit -i image.pgm", NULL);
        Image im = texte ? image_texte(texte, taille < 1 ? 1 : taille) : image_lit_pnm(entree);
        Son musique = {0};
        if (dans) { musique = wav_lit(dans); taux = musique.taux; }
        if (fmin < 0) fmin = dans ? 15000 : 500;
        if (fmax < 0) fmax = dans ? 19500 : 8000;
        if (fmax > taux / 2.0 * 0.98) meurt("fmax dépasse la moitié de la fréquence d'échantillonnage", NULL);
        if (fmin >= fmax) meurt("il faut fmin < fmax", NULL);
        if (duree <= 0) duree = im.w * 0.012 < 1 ? 1 : im.w * 0.012;
        long n = (long)(duree * taux);
        float *cache = synthetise(&im, taux, n, fmin, fmax, 40);
        Son out;
        if (dans) {
            long t0 = (long)(debut * taux);
            if (t0 + n > musique.n) meurt("l'image dépasse la fin de la musique (réduis --duree ou --debut)", NULL);
            for (long t = 0; t < n; t++)
                for (int c = 0; c < musique.canaux; c++) musique.ech[(t0 + t) * musique.canaux + c] += (float)gain * cache[t];
            out = musique;
        } else {
            for (long t = 0; t < n; t++) cache[t] *= 0.9f;
            out = (Son){taux, 1, n, cache};
        }
        wav_ecrit(sortie, &out);
        fprintf(stderr, "image %dx%d cachée entre %.0f et %.0f Hz sur %.2f s -> %s\n", im.w, im.h, fmin, fmax, duree, sortie);
        fprintf(stderr, "pour la voir : spectro reveler -i %s -o image.png --fmin %.0f --fmax %.0f\n", sortie, fmin, fmax);
    } else if (!strcmp(mode, "reveler") || !strcmp(mode, "révéler")) {
        if (!entree) meurt("il manque -i son.wav", NULL);
        if (nfft < 64 || (nfft & (nfft - 1))) meurt("--fft doit être une puissance de 2 (>= 64)", NULL);
        if (saut < 1) meurt("--saut doit être positif", NULL);
        Son s = wav_lit(entree);
        if (fmin < 0) fmin = 0;
        if (fmax < 0) fmax = s.taux / 2.0;
        Image im = spectrogramme(&s, nfft, saut, fmin, fmax, dyn);
        image_ecrit(sortie, &im);
        fprintf(stderr, "spectrogramme %dx%d (%.0f..%.0f Hz) -> %s\n", im.w, im.h, fmin, fmax, sortie);
    } else {
        aide();
        return 1;
    }
    return 0;
}
