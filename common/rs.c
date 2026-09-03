#include "rs.h"
#include <string.h>

/* GF(256) with primitive polynomial x^8 + x^4 + x^3 + x^2 + 1 (0x11D). */
static uint8_t gf_exp[512];
static uint8_t gf_log[256];
static uint8_t gen[RS_PARITY + 1]; /* generator polynomial, gen[0] = 1 (highest degree) */
static int tables_ready = 0;

static uint8_t gf_mul(uint8_t a, uint8_t b) {
    if(a == 0 || b == 0) return 0;
    return gf_exp[gf_log[a] + gf_log[b]];
}

static uint8_t gf_div(uint8_t a, uint8_t b) {
    /* b != 0 */
    if(a == 0) return 0;
    return gf_exp[gf_log[a] + 255 - gf_log[b]];
}

static uint8_t gf_pow(int e) {
    e %= 255;
    if(e < 0) e += 255;
    return gf_exp[e];
}

static void tables_init(void) {
    if(tables_ready) return;
    int x = 1;
    for(int i = 0; i < 255; i++) {
        gf_exp[i] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        x <<= 1;
        if(x & 0x100) x ^= 0x11D;
    }
    for(int i = 255; i < 512; i++) gf_exp[i] = gf_exp[i - 255];
    gf_log[0] = 0;

    /* gen(x) = prod_{i=0}^{RS_PARITY-1} (x + alpha^i), highest degree first, gen[0] = 1. */
    uint8_t g[RS_PARITY + 1];
    int deg = 0;
    g[0] = 1;
    for(int i = 0; i < RS_PARITY; i++) {
        uint8_t r = gf_pow(i);
        uint8_t ng[RS_PARITY + 1];
        ng[0] = g[0];
        for(int j = 1; j <= deg; j++) ng[j] = g[j] ^ gf_mul(r, g[j - 1]);
        ng[deg + 1] = gf_mul(r, g[deg]);
        deg++;
        memcpy(g, ng, (size_t)(deg + 1));
    }
    memcpy(gen, g, sizeof gen);
    tables_ready = 1;
}

void rs_encode(const uint8_t* data, int data_len, uint8_t* parity) {
    tables_init();
    uint8_t rem[RS_PARITY];
    memset(rem, 0, sizeof rem);
    for(int i = 0; i < data_len; i++) {
        uint8_t coef = data[i] ^ rem[0];
        for(int j = 0; j < RS_PARITY - 1; j++) rem[j] = rem[j + 1];
        rem[RS_PARITY - 1] = 0;
        if(coef) {
            for(int j = 0; j < RS_PARITY; j++) rem[j] ^= gf_mul(gen[j + 1], coef);
        }
    }
    memcpy(parity, rem, RS_PARITY);
}

/* Evaluates poly (highest degree first, length n) at x. */
static uint8_t poly_eval(const uint8_t* p, int n, uint8_t x) {
    uint8_t y = 0;
    for(int i = 0; i < n; i++) y = gf_mul(y, x) ^ p[i];
    return y;
}

static uint8_t gf_inv(uint8_t a) {
    return gf_exp[255 - gf_log[a]];
}

int rs_decode_erasures(uint8_t* block, int n, const int* erasures, int num_erasures) {
    tables_init();
    if(n <= RS_PARITY || n > 255) return -1;
    if(num_erasures < 0 || num_erasures > RS_PARITY) return -1;

    /* Syndromes S_i = R(alpha^i), i = 0..RS_PARITY-1. */
    uint8_t S[RS_PARITY];
    int nonzero = 0;
    for(int i = 0; i < RS_PARITY; i++) {
        S[i] = poly_eval(block, n, gf_pow(i));
        if(S[i]) nonzero = 1;
    }
    if(!nonzero) return 0;

    /* Erasure locator Gamma(x) = prod (1 - X_j x), lowest degree first. */
    uint8_t G[RS_PARITY + 1];
    memset(G, 0, sizeof G);
    G[0] = 1;
    int e = 0;
    for(int j = 0; j < num_erasures; j++) {
        int k = erasures[j];
        if(k < 0 || k >= n) return -1;
        int dup = 0;
        for(int q = 0; q < j; q++) dup |= (erasures[q] == k);
        if(dup) continue;
        uint8_t X = gf_pow(n - 1 - k);
        for(int d = e + 1; d > 0; d--) G[d] ^= gf_mul(G[d - 1], X);
        e++;
    }

    /* Modified syndromes Xi(x) = S(x) * Gamma(x) mod x^RS_PARITY. */
    uint8_t Xi[RS_PARITY];
    memset(Xi, 0, sizeof Xi);
    for(int i = 0; i < RS_PARITY; i++) {
        for(int j = 0; j <= e && i + j < RS_PARITY; j++) Xi[i + j] ^= gf_mul(S[i], G[j]);
    }

    /* Berlekamp-Massey on the RS_PARITY - e modified syndromes Xi[e..]: error locator. */
    uint8_t C[RS_PARITY + 1], B[RS_PARITY + 1], T[RS_PARITY + 1];
    memset(C, 0, sizeof C);
    memset(B, 0, sizeof B);
    C[0] = 1;
    B[0] = 1;
    int L = 0, m = 1;
    uint8_t b = 1;
    int nsyn = RS_PARITY - e;
    for(int k = 0; k < nsyn; k++) {
        uint8_t d = Xi[e + k];
        for(int i = 1; i <= L; i++) d ^= gf_mul(C[i], Xi[e + k - i]);
        if(d == 0) {
            m++;
        } else if(2 * L <= k) {
            memcpy(T, C, sizeof C);
            uint8_t coef = gf_div(d, b);
            for(int i = m; i <= RS_PARITY; i++) C[i] ^= gf_mul(coef, B[i - m]);
            L = k + 1 - L;
            memcpy(B, T, sizeof B);
            b = d;
            m = 1;
        } else {
            uint8_t coef = gf_div(d, b);
            for(int i = m; i <= RS_PARITY; i++) C[i] ^= gf_mul(coef, B[i - m]);
            m++;
        }
    }
    if(2 * L + e > RS_PARITY) return -1;

    /* Combined locator Psi(x) = Lambda(x) * Gamma(x), degree L + e. */
    uint8_t Psi[2 * RS_PARITY + 2];
    memset(Psi, 0, sizeof Psi);
    for(int i = 0; i <= L; i++) {
        for(int j = 0; j <= e; j++) Psi[i + j] ^= gf_mul(C[i], G[j]);
    }
    int deg = L + e;

    /* Chien search over all positions: roots of Psi(X_k^-1). */
    int pos[RS_PARITY];
    int count = 0;
    for(int k = 0; k < n; k++) {
        uint8_t xinv = gf_pow(-(n - 1 - k));
        uint8_t v = 0, xp = 1;
        for(int i = 0; i <= deg; i++) {
            v ^= gf_mul(Psi[i], xp);
            xp = gf_mul(xp, xinv);
        }
        if(v == 0) {
            if(count >= RS_PARITY) return -1;
            pos[count++] = k;
        }
    }
    if(count != deg) return -1;

    /* Omega(x) = Xi(x) * Lambda(x) mod x^RS_PARITY  (= S * Psi mod x^RS_PARITY). */
    uint8_t Om[RS_PARITY];
    memset(Om, 0, sizeof Om);
    for(int i = 0; i < RS_PARITY; i++) {
        for(int j = 0; j <= L && i + j < RS_PARITY; j++) Om[i + j] ^= gf_mul(Xi[i], C[j]);
    }

    /* Forney with first consecutive root alpha^0: Y = X * Omega(X^-1) / Psi'(X^-1). */
    int corrected = 0;
    for(int q = 0; q < count; q++) {
        int k = pos[q];
        uint8_t X = gf_pow(n - 1 - k);
        uint8_t xinv = gf_inv(X);
        uint8_t om = 0, xp = 1;
        for(int i = 0; i < RS_PARITY; i++) {
            om ^= gf_mul(Om[i], xp);
            xp = gf_mul(xp, xinv);
        }
        uint8_t dl = 0;
        xp = 1;
        for(int i = 1; i <= deg; i += 2) {
            dl ^= gf_mul(Psi[i], xp);
            xp = gf_mul(xp, gf_mul(xinv, xinv));
        }
        if(dl == 0) return -1;
        uint8_t Y = gf_mul(X, gf_div(om, dl));
        if(Y) corrected++;
        block[k] ^= Y;
    }

    for(int i = 0; i < RS_PARITY; i++) {
        if(poly_eval(block, n, gf_pow(i)) != 0) return -1;
    }
    return corrected;
}

int rs_decode(uint8_t* block, int n) {
    return rs_decode_erasures(block, n, NULL, 0);
}
