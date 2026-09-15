#include "Search.h"
#include "../util/Threads.h"
#include "../chambergenerator/ChamberGenerator.h"
#include "../chambergenerator/ChamberTypes.h"
#include "../cubiomes/biomenoise.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>

static int REGION_BLOCKS = 34 * 16;
#define CHAMBER_REACH 116
#define ANCHOR_CHUNK 64

const char *const COUNT_NAMES[COUNT_KINDS] = {"TRIAL_SPAWNERS", "VAULTS", "OMINOUS_VAULTS"};
static const char *const COUNT_WORDS[COUNT_KINDS] = {"trial spawners", "vaults", "ominous vaults"};
const char *const SHAPE_NAMES[SHAPE_COUNT] = {"CHAMBERS", "SQUARE", "RECTANGLE", "CIRCLE"};

#define TC_NUM_TEMPLATES_MAX 1024

typedef struct { int x, z, ch; } Pt;

typedef struct { signed char dx, dz; } Off;

typedef struct { int x, z; short kind, mob; } Block;

typedef struct {
    int rx, rz;
    int posX, posY, posZ;
    int npts;
    Off *pts;
} Ch;

typedef struct {
    int count, nchambers;
    int x, z;
    int shape_w, shape_h;
    int minX, minZ;
    unsigned hash;
    int anchor;
} Result;

static int MOB_OF[TC_NUM_TEMPLATES_MAX];

static int cmp_x(const void *a, const void *b) { return ((const Pt *)a)->x - ((const Pt *)b)->x; }
static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static int best_rect(Pt *p, int n, int w, int h, int *bx, int *bz) {
    int best = 0;
    int *zs = (int *)malloc(sizeof(int) * (n ? n : 1));
    qsort(p, n, sizeof(Pt), cmp_x);
    int hi = 0;
    for (int lo = 0; lo < n; lo++) {
        if (lo > 0 && p[lo].x == p[lo - 1].x) continue;
        int x0 = p[lo].x;
        while (hi < n && p[hi].x <= x0 + w - 1) hi++;
        int m = 0;
        for (int i = lo; i < hi; i++) zs[m++] = p[i].z;
        qsort(zs, m, sizeof(int), cmp_int);
        int top = 0;
        for (int a = 0; a < m; a++) {
            while (top < m && zs[top] <= zs[a] + h - 1) top++;
            if (top - a > best) { best = top - a; *bx = x0; *bz = zs[a]; }
        }
    }
    free(zs);
    return best;
}

static int count_circle(const Pt *p, int n, int cx, int cz, int64_t r2) {
    int c = 0;
    for (int i = 0; i < n; i++) {
        int64_t dx = p[i].x - cx, dz = p[i].z - cz;
        if (dx * dx + dz * dz <= r2) c++;
    }
    return c;
}

typedef struct { double key; int d; double cx, cz; } Ev;

static int cmp_ev(const void *a, const void *b) {
    const Ev *x = (const Ev *)a, *y = (const Ev *)b;
    if (x->key < y->key) return -1;
    if (x->key > y->key) return 1;
    if (x->d != y->d) return y->d - x->d;
    if (x->cx < y->cx) return -1;
    if (x->cx > y->cx) return 1;
    if (x->cz < y->cz) return -1;
    if (x->cz > y->cz) return 1;
    return 0;
}

static double pseudo_angle(double x, double z) {
    if (z >= 0) return x >= 0 ? (x + z == 0 ? 0.0 : z / (x + z)) : 1.0 - x / (-x + z);
    return x < 0 ? 2.0 - z / (-x - z) : 3.0 + x / (x - z);
}

static int best_circle(const Pt *p, int n, int r, int *bx, int *bz) {
    int64_t r2 = (int64_t)r * r;
    double R = r;
    int best = 0;
    Ev *ev = (Ev *)malloc(sizeof(Ev) * (2 * n + 2));
    for (int i = 0; i < n; i++) {
        int ne = 0, base = 1;
        for (int j = 0; j < n; j++) {
            if (j == i) continue;
            double dx = p[j].x - p[i].x, dz = p[j].z - p[i].z;
            double d2 = dx * dx + dz * dz;
            if (d2 == 0) { base++; continue; }
            if (d2 > 4.0 * R * R) continue;
            double d = sqrt(d2);
            double h = sqrt(R * R - d2 / 4.0);
            double mx = p[i].x + dx / 2.0, mz = p[i].z + dz / 2.0;
            double ux = -dz / d * h, uz = dx / d * h;
            double ex = mx - ux, ez = mz - uz, lx = mx + ux, lz = mz + uz;
            double ke = pseudo_angle(ex - p[i].x, ez - p[i].z), kl = pseudo_angle(lx - p[i].x, lz - p[i].z);
            if (ke > kl) base++;
            ev[ne].key = ke; ev[ne].d = +1; ev[ne].cx = ex; ev[ne].cz = ez; ne++;
            ev[ne].key = kl; ev[ne].d = -1; ev[ne].cx = lx; ev[ne].cz = lz; ne++;
        }
        qsort(ev, ne, sizeof(Ev), cmp_ev);
        int cur = base, local = base;
        double atx = p[i].x + R, atz = p[i].z;
        for (int k = 0; k < ne; k++) {
            cur += ev[k].d;
            if (cur > local) { local = cur; atx = ev[k].cx; atz = ev[k].cz; }
        }
        int ix = (int)floor(atx), iz = (int)floor(atz);
        for (int ox = -1; ox <= 2; ox++) {
            for (int oz = -1; oz <= 2; oz++) {
                int c = count_circle(p, n, ix + ox, iz + oz, r2);
                if (c > best) { best = c; *bx = ix + ox; *bz = iz + oz; }
            }
        }
        int c = count_circle(p, n, p[i].x, p[i].z, r2);
        if (c > best) { best = c; *bx = p[i].x; *bz = p[i].z; }
    }
    free(ev);
    return best;
}

const char *const MOB_NAMES[MOB_COUNT] = {"ZOMBIE", "HUSK", "SPIDER", "SLIME", "CAVE_SPIDER", "SILVERFISH",
                                          "BABY_ZOMBIE", "SKELETON", "STRAY", "BOGGED", "BREEZE"};
static const char *const MOB_WORDS[MOB_COUNT] = {"zombie", "husk", "spider", "slime", "cave_spider", "silverfish",
                                                 "baby_zombie", "skeleton", "stray", "bogged", "breeze"};
static const char *const MOB_TEMPLATES[MOB_COUNT] = {"zombie", "husk", "spider", "slime", "cave_spider", "silverfish",
                                                     "baby_zombie", "skeleton", "stray", "poison_skeleton", "breeze"};

static int mob_of_template(int tpl) {
    const char *name = tc_template_name(tpl);
    const char *last = strrchr(name, '/');
    last = last ? last + 1 : name;
    for (int i = 0; i < MOB_COUNT; i++) {
        if (strcmp(last, MOB_TEMPLATES[i]) == 0) return i;
    }
    return -1;
}

static int floor_shift(int v, int s) { return v >= 0 ? v >> s : ~((~v) >> s); }

static int region_of(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

static int cmp_result(const void *a, const void *b) {
    const Result *x = (const Result *)a, *y = (const Result *)b;
    if (x->count != y->count) return y->count - x->count;
    if (x->hash != y->hash) return x->hash < y->hash ? -1 : 1;
    return x->anchor - y->anchor;
}

static int wanted(const InputData *in, int kind, int mob) {
    switch (in->count_kind) {
    case COUNT_SPAWNERS: return kind == KIND_SPAWNER && !(mob >= 0 && in->exclude[mob]);
    case COUNT_VAULTS: return kind == KIND_VAULT || kind == KIND_OMINOUS_VAULT;
    default: return kind == KIND_OMINOUS_VAULT;
    }
}

static int inside(const Area *a, const Result *r, int x, int z) {
    if (a->shape == SHAPE_CIRCLE) {
        int64_t dx = x - r->x, dz = z - r->z;
        return dx * dx + dz * dz <= (int64_t)a->radius * a->radius;
    }
    return x >= r->minX && x <= r->minX + r->shape_w - 1 && z >= r->minZ && z <= r->minZ + r->shape_h - 1;
}

typedef struct {
    Ch *chs; int nch, cap;
    Off *pts; int npts, pcap;
    int n_deep;
} Row;

typedef struct {
    Ch *chs;
    int nch;
    int *grid;
    int side, rx0, rz0;
    Row *rows;
} Scan;

typedef struct {
    const InputData *in;
    Scan *s;
    volatile long next_row, rows_done;
    time_t t0;
} ScanJob;

static void scan_rows(void *arg, int worker) {
    ScanJob *job = (ScanJob *)arg;
    const InputData *in = job->in;
    Scan *s = job->s;
    TcChamber *c = (TcChamber *)malloc(sizeof(TcChamber));
    TcWork *tw = tc_work_new();
    BiomeNoise *bn = (BiomeNoise *)malloc(sizeof(BiomeNoise));
    initBiomeNoise(bn, MC_1_21_1);
    setBiomeSeed(bn, (uint64_t)in->seed, 0);
    int64_t sr2 = (int64_t)in->search_radius * in->search_radius;
    int last_pct = 0, deep_ok = tc_deep_dark_allowed();
    for (;;) {
        long i = tc_atomic_next(&job->next_row);
        if (i >= s->side) break;
        Row *row = &s->rows[i];
        memset(row, 0, sizeof *row);
        for (int j = 0; j < s->side; j++) {
            int rx = s->rx0 + (int)i, rz = s->rz0 + j;
            int chunkX, chunkZ, px, py, pz;
            tc_candidate(in->seed, rx, rz, &chunkX, &chunkZ);
            tc_start_pos(in->seed, chunkX, chunkZ, &px, &py, &pz);
            int64_t dx = px - in->center_x, dz = pz - in->center_z;
            if (dx * dx + dz * dz > sr2) continue;

            if (!deep_ok && sampleBiomeNoise(bn, NULL, floor_shift(px, 2), floor_shift(py, 2), floor_shift(pz, 2), NULL, 0) == deep_dark) {
                row->n_deep++;
                continue;
            }
            if (tc_generate_w(tw, c, in->seed, chunkX, chunkZ) != 0) {
                printf("generator overflow at region %d %d\n", rx, rz);
                continue;
            }
            if (row->nch == row->cap) {
                row->cap = row->cap ? row->cap * 2 : 16;
                row->chs = (Ch *)realloc(row->chs, sizeof(Ch) * row->cap);
            }
            if (row->npts + c->nblocks > row->pcap) {
                while (row->npts + c->nblocks > row->pcap) row->pcap = row->pcap ? row->pcap * 2 : 1024;
                row->pts = (Off *)realloc(row->pts, sizeof(Off) * row->pcap);
            }
            Ch *h = &row->chs[row->nch];
            h->rx = rx; h->rz = rz;
            h->posX = c->posX; h->posY = c->posY; h->posZ = c->posZ;
            h->pts = NULL;
            int n0 = row->npts;
            for (int b = 0; b < c->nblocks; b++) {
                const TcBlock *bl = &c->blocks[b];
                int mob = bl->kind == KIND_SPAWNER ? MOB_OF[bl->tpl] : -1;
                if (wanted(in, bl->kind, mob)) row->pts[row->npts++] = (Off){(signed char)(bl->x - c->posX), (signed char)(bl->z - c->posZ)};
            }
            h->npts = row->npts - n0;
            row->nch++;
        }
        if (row->npts) row->pts = (Off *)realloc(row->pts, sizeof(Off) * row->npts);
        long done = tc_atomic_next(&job->rows_done) + 1;
        if (worker == 0 && s->side > 20) {
            int pct = (int)(done * 100 / s->side);
            if (pct / 10 > last_pct / 10) {
                last_pct = pct;
                printf("  %d%% (%.0fs)\n", pct, difftime(time(NULL), job->t0));
            }
        }
    }
    free(bn);
    tc_work_free(tw);
    free(c);
}

static void scan_world(const InputData *in, Scan *s) {
    int rcx = region_of(in->center_x, REGION_BLOCKS), rcz = region_of(in->center_z, REGION_BLOCKS);
    int rr = in->search_radius / REGION_BLOCKS + 1;
    s->side = 2 * rr + 1;
    s->rx0 = rcx - rr;
    s->rz0 = rcz - rr;
    int64_t nreg = (int64_t)s->side * s->side;

    tc_init();
    for (int t = 0; t < TC_NUM_TEMPLATES_MAX; t++) MOB_OF[t] = t < tc_num_templates() ? mob_of_template(t) : -1;
    ScanJob *job = (ScanJob *)calloc(1, sizeof(ScanJob));
    job->in = in;
    job->s = s;
    s->rows = (Row *)calloc((size_t)s->side, sizeof(Row));
    job->t0 = time(NULL);
    printf("Scanning %" PRId64 " regions on %d thread%s...\n", nreg, in->threads, in->threads == 1 ? "" : "s");
    tc_parallel(in->threads, scan_rows, job);

    int total_ch = 0, n_deep = 0;
    for (int i = 0; i < s->side; i++) { total_ch += s->rows[i].nch; n_deep += s->rows[i].n_deep; }
    s->chs = (Ch *)malloc(sizeof(Ch) * (total_ch ? total_ch : 1));
    s->grid = (int *)malloc(sizeof(int) * (size_t)nreg);
    for (int64_t k = 0; k < nreg; k++) s->grid[k] = -1;
    s->nch = 0;
    for (int i = 0; i < s->side; i++) {
        Row *row = &s->rows[i];
        int off = 0;
        for (int k = 0; k < row->nch; k++) {
            Ch h = row->chs[k];
            h.pts = row->pts + off;
            off += h.npts;
            s->chs[s->nch] = h;
            s->grid[(int64_t)(h.rx - s->rx0) * s->side + (h.rz - s->rz0)] = s->nch++;
        }
        free(row->chs);
        row->chs = NULL;
    }
    printf("%d chambers in range (%d skipped: deep dark), %.0fs\n", s->nch, n_deep, difftime(time(NULL), job->t0));
    free(job);
}

static void scan_free(Scan *s) {
    for (int i = 0; i < s->side; i++) free(s->rows[i].pts);
    free(s->rows); free(s->grid); free(s->chs);
}

typedef struct { void *p; int n, cap; } Buf;

static void *buf_room(Buf *b, int more, size_t size) {
    if (b->n + more > b->cap) {
        while (b->n + more > b->cap) b->cap = b->cap ? b->cap * 2 : 1024;
        b->p = realloc(b->p, size * (size_t)b->cap);
    }
    return (char *)b->p + size * (size_t)b->n;
}

static int neighbours(const Scan *s, int a, int reach, Buf *out) {
    const Ch *A = &s->chs[a];
    int ai = A->rx - s->rx0, aj = A->rz - s->rz0;
    int nb = reach / REGION_BLOCKS + 1;
    out->n = 0;
    for (int di = -nb; di <= nb; di++) {
        for (int dj = -nb; dj <= nb; dj++) {
            int i = ai + di, j = aj + dj;
            if (i < 0 || j < 0 || i >= s->side || j >= s->side) continue;
            int k = s->grid[i * s->side + j];
            if (k < 0) continue;
            const Ch *B = &s->chs[k];
            int64_t dx = B->posX - A->posX, dz = B->posZ - A->posZ;
            if (dx * dx + dz * dz > (int64_t)reach * reach) continue;
            *(int *)buf_room(out, 1, sizeof(int)) = k;
            out->n++;
        }
    }
    return out->n;
}

static int gather(const Scan *s, int a, int reach, Buf *nb, Buf *out) {
    neighbours(s, a, reach, nb);
    out->n = 0;
    for (int i = 0; i < nb->n; i++) {
        int k = ((int *)nb->p)[i];
        const Ch *B = &s->chs[k];
        Pt *p = (Pt *)buf_room(out, B->npts, sizeof(Pt));
        for (int q = 0; q < B->npts; q++) p[q] = (Pt){B->posX + B->pts[q].dx, B->posZ + B->pts[q].dz, k};
        out->n += B->npts;
    }
    return out->n;
}

static void chamber_blocks(const InputData *in, const Ch *h, TcWork *tw, TcChamber *c, Buf *out) {
    int chunkX, chunkZ;
    tc_candidate(in->seed, h->rx, h->rz, &chunkX, &chunkZ);
    if (tc_generate_w(tw, c, in->seed, chunkX, chunkZ) != 0) return;
    Block *b = (Block *)buf_room(out, c->nblocks, sizeof(Block));
    for (int i = 0; i < c->nblocks; i++) {
        const TcBlock *bl = &c->blocks[i];
        b[i] = (Block){bl->x, bl->z, (short)bl->kind, (short)(bl->kind == KIND_SPAWNER ? MOB_OF[bl->tpl] : -1)};
    }
    out->n += c->nblocks;
}

static void print_breakdown(const InputData *in, const Block *all, int n, const Area *a, const Result *r) {
    int mobs[MOB_COUNT] = {0}, spawners = 0, vaults = 0, ominous = 0;
    for (int i = 0; i < n; i++) {
        const Block *p = &all[i];
        if (a && !inside(a, r, p->x, p->z)) continue;
        if (p->kind == KIND_SPAWNER) { spawners++; if (p->mob >= 0) mobs[p->mob]++; }
        else if (p->kind == KIND_VAULT) vaults++;
        else if (p->kind == KIND_OMINOUS_VAULT) { vaults++; ominous++; }
    }
    printf("    spawners %d (", spawners);
    int first = 1;
    for (int m = 0; m < MOB_COUNT; m++) {
        if (!mobs[m]) continue;
        printf("%s%d %s%s", first ? "" : ", ", mobs[m], MOB_WORDS[m], in->exclude[m] ? " [left out]" : "");
        first = 0;
    }
    printf(")  vaults %d (ominous %d)\n", vaults, ominous);
}

static int keep_best(Result *res, int n, int64_t show) {
    if (n == 0) return 0;
    qsort(res, n, sizeof(Result), cmp_result);
    int m = 0;
    for (int i = 0; i < n && m < show; i++) {
        if (m > 0 && res[i].hash == res[m - 1].hash && res[i].count == res[m - 1].count) continue;
        res[m++] = res[i];
    }
    return m;
}

typedef struct {
    const InputData *in;
    const Scan *s;
    const Area *ar;
    int reach;
    long nchunks;
    volatile long next, done;
    Buf *res;
    time_t t0;
} AreaJob;

static int area_result(const AreaJob *job, int a, Buf *nb, Buf *pts, Buf *seen, Result *r) {
    const InputData *in = job->in;
    const Area *ar = job->ar;
    int m = gather(job->s, a, job->reach, nb, pts);
    if (m == 0) return 0;
    Pt *local = (Pt *)pts->p;
    memset(r, 0, sizeof *r);
    r->anchor = a;
    if (ar->shape == SHAPE_CIRCLE) {
        r->count = best_circle(local, m, ar->radius, &r->x, &r->z);
    } else {
        int bx = 0, bz = 0, bx2 = 0, bz2 = 0;
        r->shape_w = ar->side_w; r->shape_h = ar->side_h;
        r->count = best_rect(local, m, ar->side_w, ar->side_h, &bx, &bz);
        if (ar->shape == SHAPE_RECTANGLE && ar->side_w != ar->side_h) {
            int c2 = best_rect(local, m, ar->side_h, ar->side_w, &bx2, &bz2);
            if (c2 > r->count) { r->count = c2; bx = bx2; bz = bz2; r->shape_w = ar->side_h; r->shape_h = ar->side_w; }
        }
        r->minX = bx; r->minZ = bz;
        r->x = bx + (r->shape_w - 1) / 2; r->z = bz + (r->shape_h - 1) / 2;
    }
    if (r->count < in->min_count) return 0;

    seen->n = 0;
    int *ids = (int *)buf_room(seen, m, sizeof(int));
    int nin = 0, ns = 0;
    for (int q = 0; q < m; q++) if (inside(ar, r, local[q].x, local[q].z)) ids[nin++] = local[q].ch;
    qsort(ids, nin, sizeof(int), cmp_int);
    unsigned h = 2166136261u;
    for (int t = 0; t < nin; t++) {
        if (t > 0 && ids[t] == ids[t - 1]) continue;
        h = (h ^ (unsigned)ids[t]) * 16777619u;
        ns++;
    }
    r->hash = h;
    r->nchambers = ns;
    return 1;
}

static int nearest(const Scan *s, int a, int k, int *out, int64_t *d) {
    const Ch *A = &s->chs[a];
    int ai = A->rx - s->rx0, aj = A->rz - s->rz0, n = 0;
    int slack = REGION_BLOCKS + 2 * CHAMBER_REACH;
    for (int r = 1; r <= s->side; r++) {
        int64_t lb = (int64_t)r * REGION_BLOCKS - slack;
        if (n == k && lb > 0 && lb * lb > d[k - 1]) break;
        for (int di = -r; di <= r; di++) {
            int step = di == -r || di == r ? 1 : 2 * r;
            for (int dj = -r; dj <= r; dj += step) {
                int i = ai + di, j = aj + dj;
                if (i < 0 || j < 0 || i >= s->side || j >= s->side) continue;
                int c = s->grid[i * s->side + j];
                if (c < 0) continue;
                int64_t dx = s->chs[c].posX - A->posX, dz = s->chs[c].posZ - A->posZ, dd = dx * dx + dz * dz;
                if (n == k && (dd > d[k - 1] || (dd == d[k - 1] && c > out[k - 1]))) continue;
                int p = n < k ? n++ : k - 1;
                while (p > 0 && (d[p - 1] > dd || (d[p - 1] == dd && out[p - 1] > c))) { d[p] = d[p - 1]; out[p] = out[p - 1]; p--; }
                d[p] = dd; out[p] = c;
            }
        }
    }
    return n;
}

static int group_members(const Scan *s, int a, int n, int *mem) {
    int64_t d[4];
    mem[0] = a;
    return n == 1 || nearest(s, a, n - 1, mem + 1, d) == n - 1;
}

static int group_result(const AreaJob *job, int a, Result *r) {
    const Scan *s = job->s;
    int n = job->ar->chambers, mem[4], ids[4];
    if (!group_members(s, a, n, mem)) return 0;
    memset(r, 0, sizeof *r);
    r->anchor = a;
    r->nchambers = n;
    for (int i = 0; i < n; i++) { r->count += s->chs[mem[i]].npts; ids[i] = mem[i]; }
    if (r->count < job->in->min_count) return 0;
    qsort(ids, n, sizeof(int), cmp_int);
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ (unsigned)ids[i]) * 16777619u;
    r->hash = h;
    return 1;
}

static void area_worker(void *arg, int worker) {
    AreaJob *job = (AreaJob *)arg;
    const Scan *s = job->s;
    Buf nb = {0}, pts = {0}, seen = {0};
    Buf *out = &job->res[worker];
    int64_t limit = job->in->show * 2 + 4096;
    int last_pct = 0;
    for (;;) {
        long k = tc_atomic_next(&job->next);
        if (k >= job->nchunks) break;
        int a0 = (int)(k * ANCHOR_CHUNK), a1 = a0 + ANCHOR_CHUNK < s->nch ? a0 + ANCHOR_CHUNK : s->nch;
        for (int a = a0; a < a1; a++) {
            Result r;
            int ok = job->ar->shape == SHAPE_CHAMBERS ? group_result(job, a, &r) : area_result(job, a, &nb, &pts, &seen, &r);
            if (!ok) continue;
            *(Result *)buf_room(out, 1, sizeof(Result)) = r;
            if (++out->n > limit) out->n = keep_best((Result *)out->p, out->n, job->in->show);
        }
        long done = tc_atomic_next(&job->done) + 1;
        if (worker == 0 && job->nchunks > 20) {
            int pct = (int)(done * 100 / job->nchunks);
            if (pct / 10 > last_pct / 10) {
                last_pct = pct;
                printf("  %d%% (%.0fs)\n", pct, difftime(time(NULL), job->t0));
            }
        }
    }
    free(nb.p); free(pts.p); free(seen.p);
}

static void run_area(const InputData *in, const Scan *s, const Area *ar) {
    int extent = ar->shape == SHAPE_CIRCLE ? 2 * ar->radius + 1 : (ar->side_w > ar->side_h ? ar->side_w : ar->side_h);
    AreaJob job;
    memset(&job, 0, sizeof job);
    job.in = in; job.s = s; job.ar = ar;
    job.reach = ar->shape == SHAPE_CHAMBERS ? 0 : extent + 2 * CHAMBER_REACH;
    job.nchunks = (s->nch + ANCHOR_CHUNK - 1) / ANCHOR_CHUNK;
    job.res = (Buf *)calloc((size_t)in->threads, sizeof(Buf));
    job.t0 = time(NULL);
    printf("Searching around %d chambers on %d thread%s...\n", s->nch, in->threads, in->threads == 1 ? "" : "s");
    tc_parallel(in->threads, area_worker, &job);

    Buf all = {0};
    for (int t = 0; t < in->threads; t++) {
        Buf *b = &job.res[t];
        if (b->n) memcpy(buf_room(&all, b->n, sizeof(Result)), b->p, sizeof(Result) * b->n);
        all.n += b->n;
        free(b->p);
    }
    free(job.res);
    Result *res = (Result *)all.p;
    int nres = keep_best(res, all.n, in->show);
    printf("\n");

    TcChamber *c = (TcChamber *)malloc(sizeof(TcChamber));
    TcWork *tw = tc_work_new();
    Buf nb = {0}, blocks = {0};
    for (int i = 0; i < nres && ar->shape == SHAPE_CHAMBERS; i++) {
        const Result *r = &res[i];
        int n = r->nchambers, mem[4];
        group_members(s, r->anchor, n, mem);
        const Ch *h0 = &s->chs[mem[0]];
        if (n == 1) {
            printf("Found %d %s in 1 chamber at /tp %d %d %d\n", r->count, COUNT_WORDS[in->count_kind], h0->posX, h0->posY, h0->posZ);
        } else {
            int64_t far = 0;
            for (int p = 0; p < n; p++) {
                for (int q = p + 1; q < n; q++) {
                    int64_t dx = s->chs[mem[p]].posX - s->chs[mem[q]].posX, dz = s->chs[mem[p]].posZ - s->chs[mem[q]].posZ;
                    if (dx * dx + dz * dz > far) far = dx * dx + dz * dz;
                }
            }
            printf("Found %d %s in %d chambers, up to %.0f blocks apart\n", r->count, COUNT_WORDS[in->count_kind], n, sqrt((double)far));
            for (int p = 0; p < n; p++) {
                const Ch *h = &s->chs[mem[p]];
                printf("    /tp %d %d %d  (%d %s)\n", h->posX, h->posY, h->posZ, h->npts, COUNT_WORDS[in->count_kind]);
            }
        }
        blocks.n = 0;
        for (int p = 0; p < n; p++) chamber_blocks(in, &s->chs[mem[p]], tw, c, &blocks);
        print_breakdown(in, (const Block *)blocks.p, blocks.n, NULL, NULL);
    }
    for (int i = 0; i < nres && ar->shape != SHAPE_CHAMBERS; i++) {
        const Result *r = &res[i];
        char where[160];
        if (ar->shape == SHAPE_CIRCLE)
            snprintf(where, sizeof where, "circle r=%d", ar->radius);
        else if (ar->shape == SHAPE_SQUARE)
            snprintf(where, sizeof where, "square r=%d (%d %d to %d %d)", ar->radius, r->minX, r->minZ,
                     r->minX + r->shape_w - 1, r->minZ + r->shape_h - 1);
        else
            snprintf(where, sizeof where, "%dx%d rectangle (%d %d to %d %d)", r->shape_w, r->shape_h, r->minX, r->minZ,
                     r->minX + r->shape_w - 1, r->minZ + r->shape_h - 1);
        printf("Found %d %s (%d chamber%s) in %s at /tp %d ~ %d\n", r->count, COUNT_WORDS[in->count_kind],
               r->nchambers, r->nchambers == 1 ? "" : "s", where, r->x, r->z);
        neighbours(s, r->anchor, job.reach, &nb);
        blocks.n = 0;
        for (int k = 0; k < nb.n; k++) chamber_blocks(in, &s->chs[((int *)nb.p)[k]], tw, c, &blocks);
        print_breakdown(in, (const Block *)blocks.p, blocks.n, ar, r);
    }
    if (nres == 0) printf("Nothing found with at least %" PRId64 " %s\n", in->min_count, COUNT_WORDS[in->count_kind]);
    free(nb.p); free(blocks.p); free(all.p);
    tc_work_free(tw);
    free(c);
}

int search_run(const InputData *in) {
    tc_select_version(in->version);
    REGION_BLOCKS = tc_region_blocks();
    int rr = in->search_radius / REGION_BLOCKS + 1;
    if ((int64_t)(2 * rr + 1) * (2 * rr + 1) > 50000000) { printf("Search radius too large\n"); return 1; }
    Scan s;
    scan_world(in, &s);
    run_area(in, &s, &in->area);
    scan_free(&s);
    return 0;
}
