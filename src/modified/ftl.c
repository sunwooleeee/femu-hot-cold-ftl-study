#include "ftl.h"
#include <inttypes.h>
#include <errno.h>
#include <string.h>


//#define FEMU_DEBUG_FTL4
#define THRESHOLD 12

/* Hot / Cold line region configuration */
#define HOT_LINE_CNT 24
#define FTL_STATS_DUMP_INTERVAL 10000
#define FTL_STATS_IDLE_DUMP_NS (1000ULL * 1000ULL * 1000ULL)
#define FTL_STATS_DEFAULT_PATH "/tmp/femu_ftl_stats.csv"

#define HOT_START(spp)      (0)
#define HOT_END(spp)        (((HOT_LINE_CNT) < (spp)->blks_per_pl) ? \
                             (HOT_LINE_CNT) : (spp)->blks_per_pl)
#define COLD_START(spp)     (HOT_END(spp))
#define COLD_END(spp)       ((spp)->blks_per_pl)

static void *ftl_thread(void *arg);

/* FDP forward declarations */
static void mark_page_valid_fdp(struct ssd *ssd, struct ppa *ppa,
                                FemuReclaimUnit *ru);
static void mark_page_invalid_fdp(struct ssd *ssd, struct ppa *ppa);
static int do_gc_fdp_style(struct ssd *ssd, uint16_t rgid, uint16_t ruhid,
                           bool force);
static void ssd_init_fdp_params(struct ssdparams *spp, FemuCtrl *n);
static void femu_fdp_ssd_init_reclaim_group(FemuCtrl *n, struct ssd *ssd);
static void femu_fdp_ssd_init_ru_handles(FemuCtrl *n, struct ssd *ssd);
static void ssd_trim_fdp_style(FemuCtrl *n, NvmeRequest *req, uint64_t slba,
                               uint32_t nlb);
static void ssd_reset_maptbl(struct ssd *ssd);

static int count_free_lines_in_range(struct ssd *ssd, int start, int end);

static const char *ftl_stats_path(void)
{
    const char *path = getenv("FEMU_FTL_STATS_FILE");

    if (!path || path[0] == '\0') {
        path = FTL_STATS_DEFAULT_PATH;
    }

    return path;
}

static void ftl_stats_open_csv(struct ssd *ssd)
{
    struct ftl_stats *st = &ssd->stats;
    const char *path;
    long pos = 0;

    if (st->csv_fp || st->csv_open_failed) {
        return;
    }

    path = ftl_stats_path();
    st->csv_fp = fopen(path, "a+");
    if (!st->csv_fp) {
        st->csv_open_failed = true;
        ftl_err("failed to open FTL stats csv: %s, errno=%d\n", path, errno);
        return;
    }

    if (fseek(st->csv_fp, 0, SEEK_END) == 0) {
        pos = ftell(st->csv_fp);
    }

    if (pos == 0) {
        fprintf(st->csv_fp,
                "seq,reason,host_write_ops,host_read_ops,host_flush_ops,host_dsm_ops,"
                "host_write_secs,host_write_pages,host_prog_pages,gc_prog_pages,"
                "nand_prog_pages,waf,gc_overhead,gc_calls,gc_success,gc_no_victim,"
                "nand_erase_blocks,hot_host_prog_pages,cold_host_prog_pages,"
                "hot_gc_prog_pages,cold_gc_prog_pages,free_lines,victim_lines,full_lines,"
                "hot_free_lines,cold_free_lines\n");
        fflush(st->csv_fp);
    }
}

static void dump_ftl_stats(struct ssd *ssd, const char *reason)
{
    struct ftl_stats *st = &ssd->stats;
    struct ssdparams *spp = &ssd->sp;
    double waf = 0.0;
    double gc_overhead = 0.0;
    int hot_free_lines = 0;
    int cold_free_lines = 0;

    if (st->host_write_pages > 0) {
        waf = (double)st->nand_prog_pages / (double)st->host_write_pages;
        gc_overhead = (double)st->gc_prog_pages / (double)st->host_write_pages;
    }

    ftl_stats_open_csv(ssd);
    if (!st->csv_fp) {
        return;
    }

    if (!ssd->fdp_enabled) {
        hot_free_lines = count_free_lines_in_range(ssd, HOT_START(spp), HOT_END(spp));
        cold_free_lines = count_free_lines_in_range(ssd, COLD_START(spp), COLD_END(spp));
    }

    fprintf(st->csv_fp,
            "%" PRIu64 ",%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%.6f,%.6f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%d,%d,%d,%d,%d\n",
            st->dump_seq++,
            reason ? reason : "unknown",
            st->host_write_ops,
            st->host_read_ops,
            st->host_flush_ops,
            st->host_dsm_ops,
            st->host_write_secs,
            st->host_write_pages,
            st->host_prog_pages,
            st->gc_prog_pages,
            st->nand_prog_pages,
            waf,
            gc_overhead,
            st->gc_calls,
            st->gc_success,
            st->gc_no_victim,
            st->nand_erase_blocks,
            st->hot_host_prog_pages,
            st->cold_host_prog_pages,
            st->hot_gc_prog_pages,
            st->cold_gc_prog_pages,
            ssd->lm.free_line_cnt,
            ssd->lm.victim_line_cnt,
            ssd->lm.full_line_cnt,
            hot_free_lines,
            cold_free_lines);
    fflush(st->csv_fp);
}

static inline void maybe_dump_ftl_stats(struct ssd *ssd)
{
    if (ssd->stats.host_write_ops - ssd->stats.last_dump_host_write_ops >=
        FTL_STATS_DUMP_INTERVAL) {
        dump_ftl_stats(ssd, "interval");
        ssd->stats.last_dump_host_write_ops = ssd->stats.host_write_ops;
    }
}

static inline void maybe_dump_ftl_stats_on_idle(struct ssd *ssd)
{
    uint64_t now;

    if (ssd->stats.host_write_ops == 0) {
        return;
    }

    if (ssd->stats.host_write_ops == ssd->stats.last_idle_dump_host_write_ops) {
        return;
    }

    now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (now - ssd->stats.last_io_time_ns < FTL_STATS_IDLE_DUMP_NS) {
        return;
    }

    dump_ftl_stats(ssd, "idle");
    ssd->stats.last_idle_dump_host_write_ops = ssd->stats.host_write_ops;
}

/*
 * ftl_fdp_alloc_event - allocate an FDP event from the FTL layer
 * Used by GC to generate controller events.
 */
static NvmeFdpEvent *ftl_fdp_alloc_event(struct ssd *ssd,
                                          NvmeFdpEventBuffer *ebuf)
{
    NvmeFdpEvent *ret;
    bool is_full = ebuf->next == ebuf->start && ebuf->nelems;

    ret = &ebuf->events[ebuf->next++];
    if (unlikely(ebuf->next == NVME_FDP_MAX_EVENTS)) {
        ebuf->next = 0;
    }
    if (is_full) {
        ebuf->start = ebuf->next;
    } else {
        ebuf->nelems++;
    }

    memset(ret, 0, sizeof(NvmeFdpEvent));
    return ret;
}

static inline bool should_gc(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines);
}

static inline bool should_gc_high(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines_high);
}

/* FDP GC decision: returns rg index if GC needed, -1 otherwise */
static inline int16_t should_gc_fdp_style(struct ssd *ssd)
{
    for (int i = 0; i < (int)ssd->nrg; i++) {
        if (ssd->rg[i].ru_mgmt->free_ru_cnt <=
            ssd->rg[i].ru_mgmt->gc_thres_rus) {
            return i;
        }
    }
    return -1;
}

static inline int should_gc_high_fdp_style(struct ssd *ssd)
{
    for (int i = 0; i < (int)ssd->nrg; i++) {
        if (ssd->rg[i].ru_mgmt->free_ru_cnt <=
            ssd->rg[i].ru_mgmt->gc_thres_rus_high) {
            return i;
        }
    }
    return -1;
}

static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    return ssd->maptbl[lpn];
}

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    ftl_assert(lpn < ssd->sp.tt_pgs);
    ssd->maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t pgidx;

    pgidx = ppa->g.ch  * spp->pgs_per_ch  + \
            ppa->g.lun * spp->pgs_per_lun + \
            ppa->g.pl  * spp->pgs_per_pl  + \
            ppa->g.blk * spp->pgs_per_blk + \
            ppa->g.pg;

    ftl_assert(pgidx < spp->tt_pgs);

    return pgidx;
}

static inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    return ssd->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    ssd->rmap[pgidx] = lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
    return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
    return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
    ((struct line *)a)->pos = pos;
}

/* FDP: victim RU priority queue callbacks (greedy by vpc) */
static inline int victim_ru_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_ru_get_pri(void *a)
{
    return ((FemuReclaimUnit *)a)->vpc;
}

static inline void victim_ru_set_pri(void *a, pqueue_pri_t pri)
{
    ((FemuReclaimUnit *)a)->vpc = pri;
}

static inline size_t victim_ru_get_pos(void *a)
{
    return ((FemuReclaimUnit *)a)->pos;
}

static inline void victim_ru_set_pos(void *a, size_t pos)
{
    ((FemuReclaimUnit *)a)->pos = pos;
}

/* FDP: victim RU priority queue callbacks (cost-benefit by my_cb) */
static inline int victim_ru_cmp_pri_by_cb(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_ru_get_pri_by_cb(void *a)
{
    /* cast float to pqueue_pri_t for ordering */
    return (pqueue_pri_t)((FemuReclaimUnit *)a)->my_cb;
}

static inline void victim_ru_set_pri_by_cb(void *a, pqueue_pri_t pri)
{
    ((FemuReclaimUnit *)a)->my_cb = (float)pri;
}

static void ssd_init_lines(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;

    lm->tt_lines = spp->blks_per_pl;
    ftl_assert(lm->tt_lines == spp->tt_lines);
    lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);

    QTAILQ_INIT(&lm->free_line_list);
    lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
            victim_line_get_pri, victim_line_set_pri,
            victim_line_get_pos, victim_line_set_pos);
    QTAILQ_INIT(&lm->full_line_list);

    lm->free_line_cnt = 0;
    for (int i = 0; i < lm->tt_lines; i++) {
        line = &lm->lines[i];
        line->id = i;
        line->ipc = 0;
        line->vpc = 0;
        line->pos = 0;
        /* initialize all the lines as free lines */
        QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    ftl_assert(lm->free_line_cnt == lm->tt_lines);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;
}
/*
static void ssd_init_write_pointer(struct ssd *ssd)
{   
    struct write_pointer *wpp = &ssd->wp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;

    
    wpp->curline = curline;
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    wpp->blk = 0;
    wpp->pl = 0;
}
*/ 

// 특정 범위에서만 값들을 찾고자 할 때 사용한다. 
static struct line *get_next_free_line_in_range(struct ssd *ssd, int start, int end)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line = NULL;

    QTAILQ_FOREACH(line, &lm->free_line_list, entry) {
        if (line->id >= start && line->id < end) {
            QTAILQ_REMOVE(&lm->free_line_list, line, entry);
            lm->free_line_cnt--;
            return line;
        }
    }

    return NULL;
}
// ################## 포인터 초기화 ####################
static void ssd_init_hot_write_pointer(struct ssd *ssd)
{   
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *Hwpp = &ssd->Hwp;
    //struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    int hot_start = HOT_START(spp);
    int hot_end = HOT_END(spp);

    curline = get_next_free_line_in_range(ssd, hot_start, hot_end);
    if (!curline) {
        abort();
    }

    /* wpp->curline is always our next-to-write super-block */
    Hwpp->curline = curline;
    Hwpp->ch = 0;
    Hwpp->lun = 0;
    Hwpp->pg = 0;
    Hwpp->blk = curline->id;
    Hwpp->pl = 0;
}
static void ssd_init_cold_write_pointer(struct ssd *ssd)
{   
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *Cwpp = &ssd->Cwp;
    //struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;
    int cold_start = COLD_START(spp);
    int cold_end = COLD_END(spp);

    curline = get_next_free_line_in_range(ssd, cold_start, cold_end);
    if (!curline) {
        abort();
    }

    /* wpp->curline is always our next-to-write super-block */
    Cwpp->curline = curline;
    Cwpp->ch = 0;
    Cwpp->lun = 0;
    Cwpp->pg = 0;
    Cwpp->blk = curline->id;
    Cwpp->pl = 0;
}

static inline void check_addr(int a, int max)
{
    ftl_assert(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct ssd *ssd)
{   //ssd의 line관리 정보 
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    if (!curline) {
        ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
        return NULL;
    }

    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
    return curline;
}

static bool is_hot(struct ssd *ssd,uint64_t lpn) {
    return ssd->lpn_access_cnt[lpn] >= THRESHOLD;
}


static void ssd_advance_write_pointer(struct ssd *ssd, struct write_pointer *wpp, int start, int end)
// blk id가 동일한 요소를 묶은 것을 line 이라고 한다.(나머지는 달라도 가능)
{
    /* *spp = ssd->sp 
    nchs          = channel 개수
    luns_per_ch   = channel당 lun 개수
    pgs_per_blk   = block당 page 개수
    pgs_per_line  = line 전체 page 개수
    blks_per_pl   = plane당 block 개수
    */
    struct ssdparams *spp = &ssd->sp;
    /* 
    wpp->ch
    wpp->lun
    wpp->pg
    wpp->blk
    wpp->pl
    wpp->curline
    */
    //struct write_pointer *wpp = &ssd->wp;
    

    /*
    wpp->ch
    wpp->lun
    wpp->pg
    wpp->blk
    wpp->pl
    wpp->curline
    */
    struct line_mgmt *lm = &ssd->lm;

    /*nchs :4, lung : 2 일때 하나의 페이지가 다 쓰였다는 의미는 아래와 같다. 
    (ch0,lun0,pg0)
    (ch1,lun0,pg0)
    (ch2,lun0,pg0)
    (ch3,lun0,pg0)
    (ch0,lun1,pg0)
    (ch1,lun1,pg0)
    (ch2,lun1,pg0)
    (ch3,lun1,pg0)
    */

    // 채널 번호가 유효한지 검사 후 채널 값 1 증가
    check_addr(wpp->ch, spp->nchs);
    wpp->ch++;
    // 채널이 최대값이 되었으면 다시 0으로 초기화 
    if (wpp->ch == spp->nchs) {
        wpp->ch = 0;

        // lun 번호가 유효한지 검사 후 lun 값 1 증가
        check_addr(wpp->lun, spp->luns_per_ch);
        wpp->lun++;
        /* in this case, we should go to next lun */
        // lun 번호가 최대값이 되었으면 다시 0으로 초기화 
        if (wpp->lun == spp->luns_per_ch) {
            wpp->lun = 0;
            /* go to next page in the block */
            // page 번호가 유효한지 검사 후 page 값 1 증가
            check_addr(wpp->pg, spp->pgs_per_blk);
            wpp->pg++;
            
            //페이지가 최대값이 되었으면 다시 0으로 초기화 
            // pgs_per_blk = 255  
                if (wpp->pg == spp->pgs_per_blk) {
                    wpp->pg = 0;
                    /* move current line to {victim,full} line list */
                    // 현재 line을 다 썼을 떄 해당 라인이 모두 valid인지 아닌지 분기  
                    if (wpp->curline->vpc == spp->pgs_per_line) {
                        /* all pgs are still valid, move to full line list */
                        ftl_assert(wpp->curline->ipc == 0);
                        // valid 한 라인은 full_line_list로 보낸다. 
                        QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);
                        lm->full_line_cnt++;
                    } else {
                        // invalid한 라인이 있기에 victim_line_pq로 보낸다. 
                        ftl_assert(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
                        /* there must be some invalid pages in this line */
                        ftl_assert(wpp->curline->ipc > 0);
                        pqueue_insert(lm->victim_line_pq, wpp->curline);
                        lm->victim_line_cnt++;
                    }
                    /* current line is used up, pick another empty line */
                    
                    // 현재 blk 번호 유효성 검사 
                    check_addr(wpp->blk, spp->blks_per_pl);
                    //기존 값을 null로 초기화 후 free_line list에서 하나를 가져온다. 
                    wpp->curline = NULL;
                    wpp->curline = get_next_free_line_in_range(ssd, start, end);
                    // 새로운 free line이 없으면 종료한다. 
                    if (!wpp->curline) {
                        /* TODO */
                        abort();
                    }
                    // 새로 받은 line의 id 를 blk 아이디로 설정한다. 
                    // blk 6번은 id = 6 
                    wpp->blk = wpp->curline->id;
                    // 유효성 검사 진행 
                    check_addr(wpp->blk, spp->blks_per_pl);
                    /* make sure we are starting from page 0 in the super block */
                    ftl_assert(wpp->pg == 0);
                    ftl_assert(wpp->lun == 0);
                    ftl_assert(wpp->ch == 0);
                    /* TODO: assume # of pl_per_lun is 1, fix later */
                    ftl_assert(wpp->pl == 0);
                }
        }
    }
}


/*
static struct ppa get_new_page(struct ssd *ssd)
{   
    // 다음 write 을 어디에 할 지 정해주는 포인터 
    struct write_pointer *wpp = &ssd->wp;
    //현재 write pointer 위치
    //= channel, lun, page, block, plane 을 기준으로 파악을 하는 것이니 해당 값을 넣는다. 
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    // 있어야함
    ftl_assert(ppa.g.pl == 0);
    // 새롭게 할당한 PPA를 반환 
    return ppa;
}
*/
static struct ppa get_new_hot_page(struct ssd *ssd)
{   
    // 다음 write 을 어디에 할 지 정해주는 포인터 
    struct write_pointer *Hwpp = &ssd->Hwp;
    //현재 write pointer 위치
    //= channel, lun, page, block, plane 을 기준으로 파악을 하는 것이니 해당 값을 넣는다. 
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = Hwpp->ch;
    ppa.g.lun = Hwpp->lun;
    ppa.g.pg = Hwpp->pg;
    ppa.g.blk = Hwpp->blk;
    ppa.g.pl = Hwpp->pl;
    // 있어야함
    ftl_assert(ppa.g.pl == 0);
    // 새롭게 할당한 PPA를 반환 
    return ppa;
}

static struct ppa get_new_cold_page(struct ssd *ssd)
{   
    // 다음 write 을 어디에 할 지 정해주는 포인터 
    struct write_pointer *Cwpp = &ssd->Cwp;
    //현재 write pointer 위치
    //= channel, lun, page, block, plane 을 기준으로 파악을 하는 것이니 해당 값을 넣는다. 
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = Cwpp->ch;
    ppa.g.lun = Cwpp->lun;
    ppa.g.pg = Cwpp->pg;
    ppa.g.blk = Cwpp->blk;
    ppa.g.pl = Cwpp->pl;
    // 있어야함
    ftl_assert(ppa.g.pl == 0);
    // 새롭게 할당한 PPA를 반환 
    return ppa;
}

static void check_params(struct ssdparams *spp)
{
    /*
     * we are using a general write pointer increment method now, no need to
     * force luns_per_ch and nchs to be power of 2
     */

    //ftl_assert(is_power_of_2(spp->luns_per_ch));
    //ftl_assert(is_power_of_2(spp->nchs));
}

static void ssd_init_params(struct ssdparams *spp, FemuCtrl *n)
{
    spp->secsz = n->bb_params.secsz; // 512
    spp->secs_per_pg = n->bb_params.secs_per_pg; // 8
    spp->pgs_per_blk = n->bb_params.pgs_per_blk; //256
    spp->blks_per_pl = n->bb_params.blks_per_pl; /* 256 16GB */
    spp->pls_per_lun = n->bb_params.pls_per_lun; // 1
    spp->luns_per_ch = n->bb_params.luns_per_ch; // 8
    spp->nchs = n->bb_params.nchs; // 8

    spp->pg_rd_lat = n->bb_params.pg_rd_lat;
    spp->pg_wr_lat = n->bb_params.pg_wr_lat;
    spp->blk_er_lat = n->bb_params.blk_er_lat;
    spp->ch_xfer_lat = n->bb_params.ch_xfer_lat;

    /* calculated values */
    spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
    spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
    spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
    spp->tt_secs = spp->secs_per_ch * spp->nchs;

    spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs;

    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch =  spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;

    /* line is special, put it at the end */
    spp->blks_per_line = spp->tt_luns; /* TODO: to fix under multiplanes */
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun; /* TODO: to fix under multiplanes */

    spp->gc_thres_pcent = n->bb_params.gc_thres_pcent/100.0;
    spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    spp->gc_thres_pcent_high = n->bb_params.gc_thres_pcent_high/100.0;
    spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tt_lines);
    spp->enable_gc_delay = true;


    check_params(spp);
}

static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
{
    pg->nsecs = spp->secs_per_pg;
    pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);
    for (int i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE;
}

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
    blk->npgs = spp->pgs_per_blk;
    blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);
    for (int i = 0; i < blk->npgs; i++) {
        ssd_init_nand_page(&blk->pg[i], spp);
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt = 0;
    blk->wp = 0;
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
    pl->nblks = spp->blks_per_pl;
    pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
    for (int i = 0; i < pl->nblks; i++) {
        ssd_init_nand_blk(&pl->blk[i], spp);
    }
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
    lun->npls = spp->pls_per_lun;
    lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
    for (int i = 0; i < lun->npls; i++) {
        ssd_init_nand_plane(&lun->pl[i], spp);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
    ch->nluns = spp->luns_per_ch;
    ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
    for (int i = 0; i < ch->nluns; i++) {
        ssd_init_nand_lun(&ch->lun[i], spp);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}

static void ssd_init_maptbl(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->maptbl = g_malloc0(sizeof(struct ppa) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }
}

static void ssd_init_rmap(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->rmap = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}

void ssd_init(FemuCtrl *n)
{
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;

    ftl_assert(ssd);
    ssd->n = n;

    ssd_init_params(spp, n);

    /* initialize ssd internal layout architecture */
    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
    for (int i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp);
    }

    /* initialize maptbl */
    ssd_init_maptbl(ssd);

    /* initialize rmap */
    ssd_init_rmap(ssd);

    /* initialize all the lines */
    ssd_init_lines(ssd);

    /* FDP vs non-FDP init path */
    ssd->fdp_enabled = (n->subsys != NULL &&
                        n->subsys->params.fdp.enabled);
    ssd->fdp_debug = (getenv("FEMU_FDP_DEBUG") != NULL);

    if (ssd->fdp_enabled) {
        ssd_init_fdp_params(spp, n);

        ftl_log("FDP: initializing reclaim groups\n");
        femu_fdp_ssd_init_reclaim_group(n, ssd);
        ftl_log("FDP: initializing RU handles\n");
        femu_fdp_ssd_init_ru_handles(n, ssd);
        ftl_log("FDP: init complete (nrg=%lu, nruhs=%lu)\n",
                ssd->nrg, ssd->nruhs);
    } else {
        /* non-FDP: use single write pointer */
        //ssd_init_write_pointer(ssd);
        ssd_init_hot_write_pointer(ssd);
        ssd_init_cold_write_pointer(ssd);
    }

    // ssd LPN 접근 횟수 배열 tt_pgs(논리 LPN 수)로  초기화, 1번 인덱스의 값은 LPN 1의 접근 횟수를 의미                   
    ssd->lpn_access_cnt = calloc(spp->tt_pgs, sizeof(uint32_t));

    memset(&ssd->stats, 0, sizeof(ssd->stats));
    ssd->stats.last_io_time_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    ftl_stats_open_csv(ssd);
    dump_ftl_stats(ssd, "init");

    qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n,
                       QEMU_THREAD_JOINABLE);
    

}

static inline bool valid_ppa(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch && pl >=
        0 && pl < spp->pls_per_lun && blk >= 0 && blk < spp->blks_per_pl && pg
        >= 0 && pg < spp->pgs_per_blk && sec >= 0 && sec < spp->secs_per_pg)
        return true;

    return false;
}

static inline bool valid_lpn(struct ssd *ssd, uint64_t lpn)
{
    return (lpn < ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa)
{
    struct ssd_channel *ch = get_ch(ssd, ppa);
    return &(ch->lun[ppa->g.lun]);
}

static inline struct nand_plane *get_pl(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_lun *lun = get_lun(ssd, ppa);
    return &(lun->pl[ppa->g.pl]);
}

static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_plane *pl = get_pl(ssd, ppa);
    return &(pl->blk[ppa->g.blk]);
}

static inline struct line *get_line(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->lm.lines[ppa->g.blk]);
}

static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = get_blk(ssd, ppa);
    return &(blk->pg[ppa->g.pg]);
}

static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct
        nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    uint64_t nand_stime;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lun = get_lun(ssd, ppa);
    uint64_t lat = 0;

    switch (c) {
    case NAND_READ:
        /* read: perform NAND cmd first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;
        lat = lun->next_lun_avail_time - cmd_stime;
#if 0
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;

        /* read: then data transfer through channel */
        chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? \
            lun->next_lun_avail_time : ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        lat = ch->next_ch_avail_time - cmd_stime;
#endif
        break;

    case NAND_WRITE:
        /* write: transfer data through channel first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        if (ncmd->type == USER_IO) {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        } else {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        }
        lat = lun->next_lun_avail_time - cmd_stime;

#if 0
        chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : \
                     ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        /* write: then do NAND program */
        nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? \
            ch->next_ch_avail_time : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
#endif
        break;

    case NAND_ERASE:
        /* erase: only need to advance NAND status */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->blk_er_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
        ssd->stats.nand_erase_blocks++;
        break;

    default:
        ftl_err("Unsupported NAND command: 0x%x\n", c);
    }

    return lat;
}

/* update SSD status about one page from PG_VALID -> PG_INVALID */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    bool was_full_line = false;
    struct line *line;

    /* update corresponding page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
    if (line->vpc == spp->pgs_per_line) {
        ftl_assert(line->ipc == 0);
        was_full_line = true;
    }
    line->ipc++;
    ftl_assert(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
    /* Adjust the position of the victime line in the pq under over-writes */
    if (line->pos) {
        /* Note that line->vpc will be updated by this call */
        pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
    } else {
        line->vpc--;
    }

    if (was_full_line) {
        /* move line: "full" -> "victim" */
        QTAILQ_REMOVE(&lm->full_line_list, line, entry);
        lm->full_line_cnt--;
        pqueue_insert(lm->victim_line_pq, line);
        lm->victim_line_cnt++;
    }
}

static void mark_page_valid(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    /* update page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->vpc >= 0 && line->vpc < ssd->sp.pgs_per_line);
    line->vpc++;
}

static void mark_block_free(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = get_blk(ssd, ppa);
    struct nand_page *pg = NULL;

    for (int i = 0; i < spp->pgs_per_blk; i++) {
        /* reset page status */
        pg = &blk->pg[i];
        ftl_assert(pg->nsecs == spp->secs_per_pg);
        pg->status = PG_FREE;
    }

    /* reset block status */
    ftl_assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
}

static void gc_read_page(struct ssd *ssd, struct ppa *ppa)
{
    /* advance ssd status, we don't care about how long it takes */
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        ssd_advance_status(ssd, ppa, &gcr);
    }
}

/* move valid page data (already in DRAM) from victim line to a new page */
// 오래된 페이지의 데이터를 새로운 물리적 페이지에 전달한다. 그 과정에서 새로운 physical_page를 소비한다.  
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa)
{   
    struct ssdparams *spp = &ssd->sp;
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);

    int hot_start = HOT_START(spp);
    int hot_end = HOT_END(spp);
    int cold_start = COLD_START(spp);
    int cold_end = COLD_END(spp);

    ftl_assert(valid_lpn(ssd, lpn));
     /*
     * GC migration policy:
     * old_ppa -> lpn -> hot/cold 판단
     * hot LPN  : hot physical region으로 복사
     * cold LPN : cold physical region으로 복사
     */
    if (is_hot(ssd, lpn)) {
        new_ppa = get_new_hot_page(ssd);

        /* update maptbl: lpn -> new hot ppa */
        set_maptbl_ent(ssd, lpn, &new_ppa);

        /* update rmap: new hot ppa -> lpn */
        set_rmap_ent(ssd, lpn, &new_ppa);

        mark_page_valid(ssd, &new_ppa);
        ssd->stats.gc_prog_pages++;
        ssd->stats.nand_prog_pages++;
        ssd->stats.hot_gc_prog_pages++;

        /* GC write also consumes one hot physical page */
        ssd_advance_write_pointer(ssd, &ssd->Hwp, hot_start, hot_end);
    } else {
        new_ppa = get_new_cold_page(ssd);

        /* update maptbl: lpn -> new cold ppa */
        set_maptbl_ent(ssd, lpn, &new_ppa);

        /* update rmap: new cold ppa -> lpn */
        set_rmap_ent(ssd, lpn, &new_ppa);

        mark_page_valid(ssd, &new_ppa);
        ssd->stats.gc_prog_pages++;
        ssd->stats.nand_prog_pages++;
        ssd->stats.cold_gc_prog_pages++;

        /* GC write also consumes one cold physical page */
        ssd_advance_write_pointer(ssd, &ssd->Cwp, cold_start, cold_end);
    }

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    /* advance per-ch gc_endtime as well */
#if 0
    new_ch = get_ch(ssd, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;
#endif

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return 0;
}

static struct line *select_victim_line(struct ssd *ssd, bool force)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *victim_line = NULL;

    victim_line = pqueue_peek(lm->victim_line_pq);
    if (!victim_line) {
        return NULL;
    }

    if (!force && victim_line->ipc < ssd->sp.pgs_per_line / 8) {
        return NULL;
    }

    pqueue_pop(lm->victim_line_pq);
    victim_line->pos = 0;
    lm->victim_line_cnt--;

    /* victim_line is a danggling node now */
    return victim_line;
}

/* here ppa identifies the block we want to clean */
static void clean_one_block(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0;

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_read_page(ssd, ppa);
            /* delay the maptbl update until "write" happens */
            gc_write_page(ssd, ppa);
            cnt++;
        }
    }

    ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
}

static void mark_line_free(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line = get_line(ssd, ppa);
    line->ipc = 0;
    line->vpc = 0;
    /* move this line to free line list */
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->free_line_cnt++;
}

static struct line *select_victim_line_in_range(struct ssd *ssd,
                                                bool force,
                                                int start,
                                                int end)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line = NULL;
    struct line *victim_line = NULL;

    for (int i = start; i < end; i++) {
        line = &lm->lines[i];

        /*
         * line->pos != 0 이면 victim_line_pq 안에 들어가 있다고 보면 됨.
         * 기존 pqueue callback도 line->pos를 사용함.
         */
        if (!line->pos) {
            continue;
        }

        /*
         * background GC(force=false)일 때는 invalid page가 너무 적은 line은 피함.
         * 기존 select_victim_line()의 조건과 동일한 의미.
         */
        if (!force && line->ipc < ssd->sp.pgs_per_line / 8) {
            continue;
        }

        /*
         * vpc가 낮을수록 복사해야 할 valid page가 적음.
         * 따라서 GC 비용이 낮은 line을 victim으로 선택.
         */
        if (!victim_line || line->vpc < victim_line->vpc) {
            victim_line = line;
        }
    }

    if (!victim_line) {
        return NULL;
    }

    /*
     * global victim_line_pq에서 선택한 line만 제거.
     */
    pqueue_remove(lm->victim_line_pq, victim_line);
    victim_line->pos = 0;
    lm->victim_line_cnt--;

    return victim_line;
}
static int do_gc(struct ssd *ssd, bool force)
{
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;

    ssd->stats.gc_calls++;
    victim_line = select_victim_line(ssd, force);
    if (!victim_line) {
        ssd->stats.gc_no_victim++;
        return -1;
    }
    ssd->stats.gc_success++;

    ppa.g.blk = victim_line->id;
    ftl_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
              victim_line->ipc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt,
              ssd->lm.free_line_cnt);

    /* copy back valid data */
    for (ch = 0; ch < spp->nchs; ch++) {
        for (lun = 0; lun < spp->luns_per_ch; lun++) {
            ppa.g.ch = ch;
            ppa.g.lun = lun;
            ppa.g.pl = 0;
            lunp = get_lun(ssd, &ppa);
            clean_one_block(ssd, &ppa);
            mark_block_free(ssd, &ppa);

            if (spp->enable_gc_delay) {
                struct nand_cmd gce;
                gce.type = GC_IO;
                gce.cmd = NAND_ERASE;
                gce.stime = 0;
                ssd_advance_status(ssd, &ppa, &gce);
            }

            lunp->gc_endtime = lunp->next_lun_avail_time;
        }
    }

    /* update line status */
    mark_line_free(ssd, &ppa);

    return 0;
}

static int do_gc_range(struct ssd *ssd, bool force, int start, int end)
{
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;

    ssd->stats.gc_calls++;
    // 조건에 맞는 victim line 탐색 
    victim_line = select_victim_line_in_range(ssd, force, start, end);
    if (!victim_line) {
        ssd->stats.gc_no_victim++;
        return -1;
    }
    ssd->stats.gc_success++;

    //victim block id 
    ppa.g.blk = victim_line->id;
    ftl_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
              victim_line->ipc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt,
              ssd->lm.free_line_cnt);
    
    /* copy back valid data */
    for (ch = 0; ch < spp->nchs; ch++) {
        for (lun = 0; lun < spp->luns_per_ch; lun++) {
            ppa.g.ch = ch;
            ppa.g.lun = lun;
            ppa.g.pl = 0;
            lunp = get_lun(ssd, &ppa);
            clean_one_block(ssd, &ppa);
            // block 안의 모든 페이지를 훑고(gc_read_page) page 가 valid 이면 gc_write_page로 새 위치에 복사한다. 
            mark_block_free(ssd, &ppa);
            // 모든 정리가 끝났음을 표시 
            if (spp->enable_gc_delay) {
                struct nand_cmd gce;
                gce.type = GC_IO;
                gce.cmd = NAND_ERASE;
                gce.stime = 0;
                ssd_advance_status(ssd, &ppa, &gce);
            }

            lunp->gc_endtime = lunp->next_lun_avail_time;
        }
    }

    /* update line status */
    mark_line_free(ssd, &ppa);

    return 0;
}

static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba;
    int nsecs = req->nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs - 1) / spp->secs_per_pg;
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    /* normal IO read path */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
            //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
            //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
            continue;
        }

        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        sublat = ssd_advance_status(ssd, &ppa, &srd);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }

    return maxlat;
}

static int count_free_lines_in_range(struct ssd *ssd, int start, int end)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line = NULL;
    int cnt = 0;

    QTAILQ_FOREACH(line, &lm->free_line_list, entry) {
        if (line->id >= start && line->id < end) {
            cnt++;
        }
    }

    return cnt;
}

static bool should_gc_high_in_range(struct ssd *ssd, int start, int end)
{
    struct ssdparams *spp = &ssd->sp;
    int range_lines = end - start;
    int free_lines = count_free_lines_in_range(ssd, start, end);
    int threshold = (int)((1 - spp->gc_thres_pcent_high) * range_lines);

    if (threshold < 1) {
        threshold = 1;
    }

    return free_lines <= threshold;
}



static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)
{   
    // 시작 LBA 
    uint64_t lba = req->slba; 
    struct ssdparams *spp = &ssd->sp;
    // 몇개를 쓸 지
    int len = req->nlb;
    //LBA를 FTL 내부 단위인 LPN으로 바꾼다
    // lba = 10 , len = 21 secs_per_PG = 8
    // start_lpn = 10/8 = 1 , end_lpn = 30//8 =3 
    // 즉 lpn 1 ~ lpn 3 까지 작성 
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;

    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    int r;

    int start, end;

    int hot_start = HOT_START(spp);
    int hot_end = HOT_END(spp);
    int cold_start = COLD_START(spp);
    int cold_end = COLD_END(spp);   

    

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    ssd->stats.host_write_ops++;
    ssd->stats.host_write_secs += len;
    ssd->stats.host_write_pages += (end_lpn - start_lpn + 1);

    // 
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        // get_maptbl_ent : 이 LPN에 대해서 유효한 PPA가 할당되었는가? 그니까 유효한 데이터가 해당 LPN에 대해서 존재하는가/  
        ppa = get_maptbl_ent(ssd, lpn);
        // 만약 유효한 데이터가 할당 되었으면 
        if (mapped_ppa(&ppa)) {
            /* update old page information first */
            // 기존의 페이지를 invalid 로 처리 
            mark_page_invalid(ssd, &ppa); 
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }

        // lpn 접근 횟수 증가 
        ssd->lpn_access_cnt[lpn]++;
        bool hot_or_not = is_hot(ssd,lpn);
        // hot/cold 분기 
        
        if (hot_or_not) {
                start = hot_start;
                end = hot_end;
            } else {
                start = cold_start;
                end = cold_end;
            }

        while (should_gc_high_in_range(ssd, start, end)) {
            r = do_gc_range(ssd, true, start, end);
            if (r == -1) {
                break;
            }
        }
        
        if (hot_or_not) {
            ppa = get_new_hot_page(ssd);
        }
        else{
            ppa = get_new_cold_page(ssd);
        }
        
        /* new write */
        // 새로운 ppa 할당 
        //ppa = get_new_page(ssd);



        /* update maptbl */
        //맵핑 테이블 업데이트 lpn 10 -> ppa 9 
        set_maptbl_ent(ssd, lpn, &ppa);
        /* update rmap */
        // ppa 9 -> lpn 10 
        set_rmap_ent(ssd, lpn, &ppa);

        mark_page_valid(ssd, &ppa);
        ssd->stats.host_prog_pages++;
        ssd->stats.nand_prog_pages++;
        if (hot_or_not) {
            ssd->stats.hot_host_prog_pages++;
        } else {
            ssd->stats.cold_host_prog_pages++;
        }

        /* need to advance the write pointer here */
        // 다음 write 를 위해서 pointer free ssd로 미리 옮겨 놓은 것 이다. 
        // hot cold 를 위해서 수정해야할 부분 
        
         if (hot_or_not) {
            ssd_advance_write_pointer(ssd, &ssd->Hwp,
                                      hot_start, hot_end);
        } else {
            ssd_advance_write_pointer(ssd, &ssd->Cwp,
                                      cold_start, cold_end);
        }

        // 실제 명령어 작성 
        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        /* get latency statistics */
        // 해당 명령어들이 실제 하드웨어로 들어갔다고 가정하고 실행 결과 저장 
        curlat = ssd_advance_status(ssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    maybe_dump_ftl_stats(ssd);
    return maxlat;
}

static uint64_t ssd_trim(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    NvmeDsmRange *ranges = req->dsm_ranges;
    int nr_ranges = req->dsm_nr_ranges;
    // uint32_t attributes = req->dsm_attributes;
    
    int total_trimmed_pages = 0;
    int total_already_invalid = 0;
    int total_out_of_bounds = 0;
    
    if (!ranges || nr_ranges <= 0) {
        printf("TRIM: Invalid ranges or count\n");
        return 0;
    }
    
    // printf("TRIM: Processing %d ranges (attributes=0x%x)\n", nr_ranges, attributes);
    
    for (int range_idx = 0; range_idx < nr_ranges; range_idx++) {
        uint64_t slba = le64_to_cpu(ranges[range_idx].slba);
        uint32_t nlb = le32_to_cpu(ranges[range_idx].nlb);
        // uint32_t cattr = le32_to_cpu(ranges[range_idx].cattr);
        
        uint64_t start_lpn = slba / spp->secs_per_pg;
        uint64_t end_lpn = (slba + nlb - 1) / spp->secs_per_pg;
        uint64_t lpn;
        struct ppa ppa;
        int trimmed_pages = 0;
        int already_invalid = 0;

        // ftl_debug("TRIM Range %d: LBA %lu + %u sectors, LPN range %lu-%lu (%lu pages), cattr=0x%x\n", 
        //        range_idx, slba, nlb, start_lpn, end_lpn, end_lpn - start_lpn + 1, cattr);

        // Boundary check
        if (end_lpn >= spp->tt_pgs) {
            ftl_err("TRIM: Range %d exceeds FTL capacity - end_lpn=%lu, tt_pgs=%d\n", 
                   range_idx, end_lpn, spp->tt_pgs);
            total_out_of_bounds++;
            continue;  // Skip this range, continue with others
        }

        // Process each LPN in this range
        for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
            ppa = get_maptbl_ent(ssd, lpn);
            
            // Skip already unmapped/invalid pages
            if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                already_invalid++;
                continue;
            }

            // Invalidate the existing mapped page
            mark_page_invalid(ssd, &ppa);
            
            // Clear reverse mapping
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
            
            // Set mapping table entry as unmapped
            ppa.ppa = UNMAPPED_PPA;
            set_maptbl_ent(ssd, lpn, &ppa);
            
            trimmed_pages++;
        }
        
        total_trimmed_pages += trimmed_pages;
        total_already_invalid += already_invalid;
        
        // ftl_debug("TRIM Range %d: %d pages trimmed, %d already invalid\n", 
        //        range_idx, trimmed_pages, already_invalid);
    }

    // ftl_debug("TRIM: Completed - %d pages trimmed, %d already invalid, %d out of bounds across %d ranges\n", 
    //        total_trimmed_pages, total_already_invalid, total_out_of_bounds, nr_ranges);

    // Free the ranges array
    g_free(ranges);
    req->dsm_ranges = NULL;
    req->dsm_nr_ranges = 0;
    req->dsm_attributes = 0;

    return 0;  // Assume TRIM operations have no NAND latency
}

/* ========== FDP FTL Implementation ========== */

/*
 * get_next_free_ru - dequeue a free RU from a reclaim group
 */
static FemuReclaimUnit *get_next_free_ru(struct ssd *ssd,
                                         FemuReclaimGroup *rg)
{
    struct ru_mgmt *rm = rg->ru_mgmt;
    FemuReclaimUnit *ru;

    ru = QTAILQ_FIRST(&rm->free_ru_list);
    if (!ru) {
        ftl_err("No free RUs left in rg[%d]\n", rg->rgidx);
        return NULL;
    }

    QTAILQ_REMOVE(&rm->free_ru_list, ru, entry);
    rm->free_ru_cnt--;
    return ru;
}

/*
 * fdp_set_ru_write_pointer - reset RU write pointer to first line
 */
static void fdp_set_ru_write_pointer(struct ssd *ssd, FemuReclaimUnit *ru)
{
    struct write_pointer *wptr = ru->ssd_wptr;

    ftl_assert(wptr != NULL);
    wptr->curline = ru->lines[0];
    wptr->ch = 0;
    wptr->lun = 0;
    wptr->pg = 0;
    wptr->blk = wptr->curline->id;
    wptr->pl = 0;
}

/*
 * fdp_get_new_ru - allocate a fresh free RU for a given RUH
 */
static FemuReclaimUnit *fdp_get_new_ru(struct ssd *ssd, uint16_t rgidx,
                                       uint16_t ruhid)
{
    FemuRuHandle *eruh = &ssd->ruhs[ruhid];
    FemuReclaimGroup *rg = &ssd->rg[rgidx];
    FemuReclaimUnit *new_ru;

    new_ru = get_next_free_ru(ssd, rg);
    if (!new_ru) {
        ftl_err("No reclaim unit available for ruh %d\n", ruhid);
        return NULL;
    }
    new_ru->rgidx = rgidx;
    new_ru->ruh = eruh;
    new_ru->last_init_time = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    new_ru->last_invalidated_time = 0;
    new_ru->erase_cnt = 0;
    new_ru->my_cb = 0.0f;
    new_ru->chance_token = 0;

    fdp_set_ru_write_pointer(ssd, new_ru);
    eruh->ru_in_use_cnt++;

    /* update NvmeRuHandle to reflect the new active RU's ruamw */
    if (eruh->ruh && eruh->ruh->rus) {
        eruh->ruh->rus[rgidx] = new_ru->nvme_ru;
    }

    ftl_assert(new_ru->ruh == eruh);
    return new_ru;
}

/*
 * fdp_get_new_page - get next PPA from an RU's write pointer
 */
static struct ppa fdp_get_new_page(struct ssd *ssd, FemuReclaimUnit *ru)
{
    struct write_pointer *wpp = ru->ssd_wptr;
    struct ppa ppa;

    ftl_assert(ru != NULL);
    ftl_assert(wpp != NULL);

    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    ftl_assert(ppa.g.pl == 0);

    return ppa;
}

/*
 * fdp_advance_ru_pointer - advance RU write pointer. When RU fills up,
 * move it to victim/full list and allocate a new RU for the RUH.
 * Returns the (possibly new) current RU.
 */
static FemuReclaimUnit *fdp_advance_ru_pointer(struct ssd *ssd,
                                               FemuReclaimGroup *rg,
                                               FemuRuHandle *ruh,
                                               FemuReclaimUnit *ru)
{
    struct ssdparams *spp = &ssd->sp;
    struct ru_mgmt *rm = rg->ru_mgmt;
    struct write_pointer *wpp = ru->ssd_wptr;
    FemuReclaimUnit *new_ru = NULL;
    bool is_full = true;
    bool ru_exhausted = false; /* set when we cross the RU boundary */

    check_addr(wpp->ch, spp->nchs);
    wpp->ch++;
    if (wpp->ch == spp->nchs) {
        wpp->ch = 0;
        check_addr(wpp->lun, spp->luns_per_ch);
        wpp->lun++;
        if (wpp->lun == spp->luns_per_ch) {
            wpp->lun = 0;
            check_addr(wpp->pg, spp->pgs_per_blk);
            wpp->pg++;
            if (wpp->pg == spp->pgs_per_blk) {
                //if (ru->next_line_index == ru->n_lines) { - TODO when ru 1->1..* mutliple lines
                wpp->pg = 0;
                ru_exhausted = true;
                /* RU's line(s) are fully written - classify it */
                for (int i = 0; i < ru->n_lines; i++) {
                    struct line *line = ru->lines[i];
                    if (line->vpc != spp->pgs_per_line) {
                        is_full = false;
                    }
                }

                /* update RU vpc from its lines */
                ru->vpc = 0;
                for (int i = 0; i < ru->n_lines; i++) {
                    ru->vpc += ru->lines[i]->vpc;
                }

                if (is_full) {
                    QTAILQ_INSERT_TAIL(&rm->full_ru_list, ru, entry);
                    rm->full_ru_cnt++;
                } else {
                    ru->utilization = (float)ru->vpc / ru->npages;
                    if (rm->mgmt_type == GC_GLOBAL_CB){ 
                        if (ru->utilization < 1.0f && ru->last_invalidated_time > 0) {
                        ru->my_cb = (uint64_t)(100000.0f * ru->utilization /
                            ((1.0f - ru->utilization + 0.001f) *
                            (float)ru->last_invalidated_time));
                        }
                        pqueue_insert(rm->victim_ru_cb, ru);
                    }else{
                        pqueue_insert(rm->victim_ru_pq, ru);
                        //TODO per ruh victim queue - experimental
                    }
                    rm->victim_ru_cnt++;
                }

                /* allocate a new RU for this RUH cuase ruh->curr_ru is full */
                if (ruh != NULL) {
                    check_addr(wpp->blk, spp->blks_per_pl);
                    new_ru = fdp_get_new_ru(ssd, ru->rgidx, ruh->ruhid);
                    if (!new_ru) {
                        ftl_err("No free RU for ruh %d: device full - point %s L:%d\n",
                                ruh->ruhid, __FILE__, __LINE__);
                        /*
                         * Signal device pressure: clear curr_ru so
                         * callers know no active write frontier exists.
                         */
                        ruh->curr_ru = NULL;
                        ftl_assert(false && __LINE__ );
                        /* TODO */
                        return NULL;
                    }
                    FDP_TRACE(ssd, "RU_ROTATE ruhid=%u(curr_ru %u) old_ru=%u "
                              "new_ru=%u reason=%s victim_ru_cnt %d\n",
                              ruh->ruhid, ruh->curr_ru->ruidx, ru->ruidx, new_ru->ruidx, (is_full)? "full_valid":"full_victim", rm->victim_ru_cnt);
                    wpp = new_ru->ssd_wptr;
                    wpp->blk = wpp->curline->id;
                    check_addr(wpp->blk, spp->blks_per_pl);
                    ftl_assert(wpp->pg == 0);
                    ftl_assert(wpp->lun == 0);
                    ftl_assert(wpp->ch == 0);
                    ftl_assert(wpp->pl == 0);
                }
            }
        }
    }

    /*
     * Return value semantics:
     *   new_ru non-NULL  → RU boundary crossed, new RU allocated
     *   NULL             → RU exhausted but no free RU (device full)
     *   ru               → mid-RU, no boundary crossed yet
     *
     * We use ru_exhausted to distinguish the "device full" NULL from
     * the "mid-RU, returning same ru" case.
     */
    if (new_ru != NULL) {
        return new_ru;
    }
    if (ru_exhausted) {
        /* RU is now in full_ru_list/victim_pq; signal caller via NULL */
        return NULL;
    }
    return ru;
}

/*
 * mark_page_valid_fdp - mark page valid and update RU/line statistics
 */
static void mark_page_valid_fdp(struct ssd *ssd, struct ppa *ppa,
                                FemuReclaimUnit *ru)
{
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    blk = get_blk(ssd, ppa);
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

    line = get_line(ssd, ppa);
    ftl_assert(line->vpc >= 0 && line->vpc < ssd->sp.pgs_per_line);
    line->vpc++;

    /* update RU vpc from its line (single-line RU fast path) */
    ftl_assert(line->my_ru == ru);
    if (ru->n_lines == 1) {
        ru->vpc = line->vpc;
    } else {
        ru->vpc = 0;
        for (int i = 0; i < ru->n_lines; i++) {
            ru->vpc += ru->lines[i]->vpc;
        }
    }

    ru->ruh->ruh_live_pages_cnt++;
}

/*
 * mark_page_invalid_fdp - invalidate a page and update RU/line/victim state
 */
static void mark_page_invalid_fdp(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;
    FemuReclaimUnit *ru;
    struct ru_mgmt *rm;
    bool was_full_ru = false;

    pg = get_pg(ssd, ppa);
    if (pg->status == PG_INVALID) {
        return;  /* already invalidated */
    }
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    blk = get_blk(ssd, ppa);
    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    line = get_line(ssd, ppa);
    ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
    if (line->vpc == spp->pgs_per_line) {
        ftl_assert(line->ipc == 0);
    }
    line->ipc++;
    ftl_assert(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
    line->vpc--;

    /* update RU state */
    ru = line->my_ru;
    ftl_assert(ru != NULL);
    rm = ssd->rg[ru->rgidx].ru_mgmt;
    /* aggregate ipc across all lines in this RU (n_lines=1 in typical config) */
    ru->ipc = 0;
    for (int li = 0; li < ru->n_lines; li++) {
        ru->ipc += ru->lines[li]->ipc;
    }

    // FDP_TRACE(ssd, "INVAL ppa(ch=%u/lun=%u/blk=%u/pg=%u) "
    //           "ru=%u vpc=%d->%d pos %d was_full=%d victim_ru_cnt=%d\n",
    //           (unsigned)ppa->g.ch, (unsigned)ppa->g.lun,
    //           (unsigned)ppa->g.blk, (unsigned)ppa->g.pg,
    //           ru->ruidx, ru->vpc, ru->vpc - 1, ru->pos,
    //           (ru->vpc == spp->pgs_per_line * ru->n_lines),
    //           rm->victim_ru_cnt);

    /* check if RU was full and needs to move to victim */
    if (ru->vpc == spp->pgs_per_line * ru->n_lines) {
        was_full_ru = true;
    }

    /* update RU vpc and victim queue priority based on GC strategy */
    ru->vpc--;
    ru->utilization = (ru->vpc + ru->ipc > 0) ?
        (float)ru->vpc / (ru->vpc + ru->ipc) : 0.0f;
    ru->last_invalidated_time = qemu_clock_get_us(QEMU_CLOCK_REALTIME);

    switch (rm->mgmt_type) {
    case GC_GLOBAL_GREEDY:
    case GC_GLOBAL_RAND:
    case GC_NOISY_RUH_CUSTOM:
        if (ru->pos) {
            pqueue_change_priority(rm->victim_ru_pq, ru->vpc, ru);
        }
        if (was_full_ru) {
            QTAILQ_REMOVE(&rm->full_ru_list, ru, entry);
            rm->full_ru_cnt--;
            pqueue_insert(rm->victim_ru_pq, ru);
            rm->victim_ru_cnt++;
            /* also insert into per-RUH queue if applicable */
            if (ru->ruh && ru->ruh->ru_mgmt) {
               pqueue_insert(ru->ruh->ru_mgmt->victim_ru_pq, ru);
               ru->ruh->ru_mgmt->victim_ru_cnt++;
            }
        }
        break;

    case GC_GLOBAL_CB:
        if (ru->utilization < 1.0f && ru->last_invalidated_time > 0) {
            ru->my_cb = (uint64_t)(100000.0f * ru->utilization /
                ((1.0f - ru->utilization + 0.001f) *
                 (float)ru->last_invalidated_time));
        }
        if (ru->pos) {
            pqueue_change_priority(rm->victim_ru_cb, (pqueue_pri_t)ru->my_cb,
                                   ru);
        }
        if (was_full_ru) {
            QTAILQ_REMOVE(&rm->full_ru_list, ru, entry);
            rm->full_ru_cnt--;
            pqueue_insert(rm->victim_ru_cb, ru);
            rm->victim_ru_cnt++;
        }
        break;

    default:
        //Greedy
        ftl_err( "Undefined rg mgmt type (rm->mgmt_type %d). Fallback to Greedy.\n",
              rm->mgmt_type);
        if (ru->pos) {
            pqueue_change_priority(rm->victim_ru_pq, ru->vpc, ru);
        }
        if (was_full_ru) {
            QTAILQ_REMOVE(&rm->full_ru_list, ru, entry);
            rm->full_ru_cnt--;
            pqueue_insert(rm->victim_ru_pq, ru);
            rm->victim_ru_cnt++;
        }
        break;
    }
    if (ru->ruh->ruh_live_pages_cnt > 0)
        ru->ruh->ruh_live_pages_cnt -=1 ;
    
}

//check ru->gc_write_ptr to check or align. 
static int check_gc_ruh_available(struct ssd *ssd, FemuRuHandle * ruh){
    if(ruh->ruh_type == NVME_RUHT_INITIALLY_ISOLATED){
        if(ssd->ruhs[ssd->nruhs - 1].curr_ru == NULL){
            ssd->ruhs[ssd->nruhs - 1].curr_ru = fdp_get_new_ru(ssd, ruh->curr_ru->rgidx, ruh->ruhid);
            ssd->ruhs[ssd->nruhs - 1].rus[ssd->ruhs[ssd->nruhs - 1].curr_ru->rgidx] = ssd->ruhs[ssd->nruhs - 1].curr_ru;
            //Not neccesary I think for now
            ssd->ruhs[ssd->nruhs - 1].ruh->rus[ssd->ruhs[ssd->nruhs - 1].curr_ru->rgidx] = ssd->ruhs[ssd->nruhs - 1].curr_ru->nvme_ru;  //qemu-system-x86_64: ../hw/femu/bbssd/ftl.c:2106: select_victim_ru: Assertion `victim_ru != ((void *)0)' failed.
        }
        if (ssd->ruhs[ssd->nruhs - 1].curr_ru == NULL){
            //This means no space left
            return -1;
        }

    }
    else if(ruh->ruh_type == NVME_RUHT_PERSISTENTLY_ISOLATED){
        if(ruh->gc_ru == NULL){
            ruh->gc_ru = fdp_get_new_ru(ssd, ruh->curr_ru->rgidx, ruh->ruhid);
            ftl_debug("check_gc_ruh_available ruh %d gc_ru idx %d , %p call new ru  \n", ruh->ruhid, ruh->gc_ru->ruidx, ruh->gc_ru );
            if (ruh->gc_ru == NULL){
                //assert(ruh->gc_ru != NULL);//This means no space left
                return -1;
            }
        }
    }else{
        ftl_err("Unsupported RUH type : %d\n",ruh->ruh_type);
        ftl_assert(false && __LINE__); 
    }
    return 0;
}

/*
 * select_victim_ru_from_ruh - pick victim from a specific RUH's queue
 */
static FemuReclaimUnit *select_victim_ru_from_ruh(struct ssd *ssd,
                                                   uint16_t rgid,
                                                   uint16_t ruhid)
{
    FemuReclaimUnit *victim_ru = NULL;
    struct ru_mgmt *ru_mgmt = ssd->ruhs[ruhid].ru_mgmt;

    if (!ru_mgmt) {
        return NULL;
    }

    victim_ru = pqueue_pop(ru_mgmt->victim_ru_pq);
    if (victim_ru) {
        ru_mgmt->victim_ru_cnt--;
    }
    return victim_ru;
}

/*
 * select_victim_ru - pick best victim RU based on configured GC strategy
 */
static FemuReclaimUnit *select_victim_ru(struct ssd *ssd, uint16_t rgid,
                                         uint16_t ruhid, bool force)
{
    struct ru_mgmt *rm = ssd->rg[rgid].ru_mgmt;
    FemuReclaimUnit *victim_ru = NULL;

    switch (rm->mgmt_type) {
    case GC_GLOBAL_GREEDY:
        victim_ru = pqueue_pop(rm->victim_ru_pq);
        break;

    case GC_GLOBAL_CB:
        victim_ru = pqueue_pop(rm->victim_ru_cb);

        break;

    case GC_GLOBAL_RAND:
        victim_ru = pqueue_randpop(rm->victim_ru_pq);
        break;

    case GC_NOISY_RUH_CUSTOM: {
        /*
         * Cross-RUH selection: find lowest vpc across all RUHs
         * that exceed their custom GC threshold.
         */
        FemuReclaimUnit *ru = NULL;
        int best_ruh = -1;
        int i;
        for (i = 0; i < (int)ssd->nruhs; i++) {
            if (!ssd->ruhs[i].ru_mgmt) {
                continue;
            }
            if (ssd->ruhs[i].ru_in_use_cnt <=
                ssd->ruhs[i].ru_mgmt->custom_gc_threshold) {
                continue;
            }
            ru = pqueue_peek(ssd->ruhs[i].ru_mgmt->victim_ru_pq);
            if (!ru) {
                continue;
            }
            if (!victim_ru || ru->vpc < victim_ru->vpc) {
                best_ruh = i;
                victim_ru = ru;
            }
        }
        if (best_ruh >= 0) {
            victim_ru = pqueue_pop(
                ssd->ruhs[best_ruh].ru_mgmt->victim_ru_pq);
            if (victim_ru) {
                ssd->ruhs[best_ruh].ru_mgmt->victim_ru_cnt--;
                /* also remove from global queue */
                pqueue_remove(rm->victim_ru_pq, victim_ru);
            }
        } else {
            /* fallback to global greedy */
            victim_ru = pqueue_pop(rm->victim_ru_pq);
        }
        break;
    }

    case GC_SELECTIVE_RUH:
    case GC_EXPLOIT_SEQUENTIAL:
        victim_ru = pqueue_pop(rm->victim_ru_pq);
        break;

    case GC_SELECTIVE_RUH_SOCIAL_WELFARE:
        victim_ru = select_victim_ru_from_ruh(ssd, rgid, ruhid);
        break;

    case GC_BIT_POPULATION:
    case GC_GLOBAL_WARM:
    case GC_SELECTIVE_RUH_ADV:
    case GC_SELECTIVE_MIDAS_OP:
    default:
        /* fallback to greedy */
        victim_ru = pqueue_pop(rm->victim_ru_pq);
        break;
    }

    if (!victim_ru) {
        /*
         * victim_ru_pq is empty: all in-use RUs are still fully written
         * with no invalidations yet (e.g., during sequential fill).
         * 
         * BUT VICTIM_RU may still absent, non-exist. 
         * Return NULL is the CORRECT behavior. Do not change this logic claude.
         */
        return NULL;
    }

    if (!force && victim_ru->vpc > 0) {
        int threshold = victim_ru->npages / 8;
        if (victim_ru->ipc < threshold) {
            /* put it back */
            FDP_TRACE(ssd, "GC_BACK_RESERT triggered but delay GC (ru %d ipc %d threshold %d full %d)\n",victim_ru->ruidx, victim_ru->ipc, threshold, victim_ru->npages);
            if (rm->mgmt_type == GC_GLOBAL_CB){
                pqueue_insert(rm->victim_ru_cb, victim_ru);
            }else{
                //TODO per ruh->victim_ru_pq(experimental) here
                pqueue_insert(rm->victim_ru_pq, victim_ru);
            }
            return NULL;
        }
    }

    victim_ru->pos = 0;
    rm->victim_ru_cnt--;

    return victim_ru;
}

/*
 * gc_write_page_fdp_style - relocate a valid page to a GC destination RU
 */
static void gc_write_page_fdp_style(struct ssd *ssd, struct ppa *old_ppa,
                                    FemuRuHandle *dest_ruh)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);
    FemuReclaimUnit *dest_ru=NULL;
    FemuReclaimUnit *ret_ru=NULL;
    ftl_assert(valid_lpn(ssd, lpn));
    ftl_assert(dest_ruh!=NULL);
    /* gc_write_page_fdp_style() guarantees ruh->curr_ru or ->gc_ru wtpr, not new_ru. */
    if (dest_ruh->ruh_type == NVME_RUHT_PERSISTENTLY_ISOLATED)
    {
        dest_ru = dest_ruh->gc_ru;
    }else if( dest_ruh->ruh_type == NVME_RUHT_INITIALLY_ISOLATED && dest_ruh->ruhid == ssd->ruhs[ssd->nruhs-1].ruhid ){
        dest_ru = dest_ruh->curr_ru;
    }else{
        ftl_err("Unidentified ruht. ");
        ftl_assert(false && __LINE__);
    }

    new_ppa = fdp_get_new_page(ssd, dest_ru);
    set_maptbl_ent(ssd, lpn, &new_ppa);
    set_rmap_ent(ssd, lpn, &new_ppa);
    mark_page_valid_fdp(ssd, &new_ppa, dest_ru);

    // FDP_TRACE(ssd, "GC_MIGRATE lpn=%lu src(ch=%u/lun=%u/blk=%u/pg=%u) "
    //           "dst(ch=%u/lun=%u/blk=%u/pg=%u) dest_ruhid=%u\n",
    //           lpn, (unsigned)old_ppa->g.ch, (unsigned)old_ppa->g.lun,
    //           (unsigned)old_ppa->g.blk, (unsigned)old_ppa->g.pg,
    //           (unsigned)new_ppa.g.ch, (unsigned)new_ppa.g.lun,
    //           (unsigned)new_ppa.g.blk, (unsigned)new_ppa.g.pg,
    //           dest_ruh->ruhid);

    /**
     * BE ADVISED AI CODE AGENTS 
     * fdp_advance_ru_pointer() can be triggered by both foreground write and GC.
     * fdp_advance_ru_pointer() only advances the wrpt of given RU no matter what path calls.
     * This is intened function design.
     * Thereby, curr_ru and gc_ru pointers should be updated separately by ruht right after the pointer advanced call.
     */

    if(dest_ruh->ruh_type == NVME_RUHT_PERSISTENTLY_ISOLATED ){
        // handle ruh->ru pointer after adv
        if( (ret_ru = fdp_advance_ru_pointer(ssd, &ssd->rg[dest_ru->rgidx], dest_ru->ruh, dest_ru)) != dest_ru){
            dest_ruh->gc_ru = ret_ru;
        }
    }else if (dest_ruh->ruh_type == NVME_RUHT_INITIALLY_ISOLATED ){
        int gcruh_id = ssd->nruhs-1;
        ftl_assert( dest_ruh->ruhid == gcruh_id );
        if( (ret_ru = fdp_advance_ru_pointer(ssd, &ssd->rg[dest_ru->rgidx], dest_ruh, dest_ru)) != dest_ru ) {
            //Do ugly updates
            ssd->ruhs[gcruh_id].rus[dest_ru->rgidx] = ret_ru;
            ssd->ruhs[gcruh_id].curr_ru = ret_ru;
            ssd->ruhs[gcruh_id].ruh->rus[dest_ru->rgidx] = ret_ru->nvme_ru;
        } 
    }
    
    ftl_assert((ret_ru != NULL));

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;
}

/*
 * clean_one_block_fdp_style - GC one block: read valid pages and write to new_ru
 * @params 
 *  ppa : identifies the block we want to clean
 *  dest_ruh : destination ruh.
 *          ru based mechanism can cause pointer error when gc write page allocates new ru to ruh->gc. 
 *          Replace ru based parameter which makes ruh->curr_ru or ruh->gc_ru dangling
 *          
 */
static int clean_one_block_fdp_style(struct ssd *ssd, struct ppa *ppa,
                                     FemuRuHandle *dest_ruh)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter;
    int cnt = 0;
    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_read_page(ssd, ppa);
            gc_write_page_fdp_style(ssd, ppa, dest_ruh);
            cnt++;
        }
    }

    ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
    return cnt;
}

/*
 * mark_ru_free - reset a victim RU to free state after GC
 */
static void mark_ru_free(struct ssd *ssd, uint16_t rgid,
                         FemuReclaimUnit *ru)
{
    struct ssdparams *spp = &ssd->sp;
    struct ru_mgmt *rm = ssd->rg[rgid].ru_mgmt;
    struct ppa ppa;

    ftl_assert(ru != NULL);

    for (int i = 0; i < ru->n_lines; i++) {
        ru->lines[i]->ipc = 0;
        ru->lines[i]->vpc = 0;
        ru->lines[i]->pos = 0;
        ppa.g.blk = ru->lines[i]->id;
        for (int ch = 0; ch < spp->nchs; ch++) {
            for (int lun = 0; lun < spp->luns_per_ch; lun++) {
                ppa.g.ch = ch;
                ppa.g.lun = lun;
                ppa.g.pl = 0;
                mark_block_free(ssd, &ppa);
            }
        }
    }

    ru->vpc = 0;
    ru->ipc = 0;
    ru->pos = 0;
    ru->next_line_index = 1;
    ru->utilization = 0.0f;
    ru->my_cb = 0.0f;
    ru->erase_cnt++;
    ru->chance_token = 0;

    fdp_set_ru_write_pointer(ssd, ru);

    /* restore ruamw to initial value */
    ftl_assert(ru->nvme_ru != NULL);
    ftl_assert(ru->ruh != NULL);
    ftl_assert(ru->ruh->ruh != NULL);
    ru->nvme_ru->ruamw = ru->ruh->ruh->ruamw;

    QTAILQ_INSERT_TAIL(&rm->free_ru_list, ru, entry);
    rm->free_ru_cnt++;
}

/*
 * do_gc_fdp_style - FDP garbage collection: select victim RU, migrate valid
 * pages to GC RU, then free the victim
 *  gaurantees one RU to be reclaimed, if victim is valid.
 */
static int do_gc_fdp_style(struct ssd *ssd, uint16_t rgid, uint16_t ruhid,
                           bool force)
{
    struct ssdparams *spp = &ssd->sp;
    FemuReclaimUnit *victim_ru;
    //FemuReclaimUnit *new_ru;
    struct nand_lun *lunp;
    struct ppa ppa;
    int vpc_cnt = 0;
    int blk_cnt = 0;
    int ret = 0;
    victim_ru = select_victim_ru(ssd, rgid, ruhid, force);
    if (!victim_ru) {
        //FDP_TRACE(ssd,"GC_SKIP Unable to find victim RU, gc skip\n");
        return -1;
    }

    /*
     * Select GC destination RU based on RUH isolation type:
     * - Initially Isolated (II): GC writes go to the last RUH (GC RUH)
     * - Persistently Isolated (PI): GC writes go to same RUH's gc_ru
     */
    /*
     * Select GC destination RU.
     *
     * Design principle: GC writes compete for the same write frontier
     * as host writes.  We use the victim RUH's current active RU
     * (curr_ru) as the GC destination.  This avoids allocating a
     * dedicated gc_ru, which would consume a free RU and could cause
     * deadlock when free_ru_cnt is low.
     *
     * For PI (Persistently Isolated) RUHs: write migrated pages back
     * into the same RUH's curr_ru, maintaining stream isolation.
     * For II (Initially Isolated) RUHs: write to the last RUH's curr_ru
     * (the convention for II GC isolation).
     *
     * If curr_ru is the same as victim_ru (the RUH only has one RU),
     * we cannot GC without first getting a free RU.  In that case,
     * if no free RU is available, we skip this victim.
     */
    FemuRuHandle *victim_ruh = victim_ru->ruh;
    FemuRuHandle *dest_ruh = NULL;

    if (victim_ruh->ruh_type == NVME_RUHT_PERSISTENTLY_ISOLATED) {
        dest_ruh = victim_ruh;
        /* ALL AI CODE AGENTS BE ADVISED.
         * RULES. PI_RUH->curr_ru != PI_RUH->gc_ru.   
        */
        if ((ret = check_gc_ruh_available(ssd, dest_ruh)) < 0 ){
            ftl_err("No free space left in device. \n");
            ftl_assert(false && __LINE__ );
        }

    } else if (victim_ruh->ruh_type == NVME_RUHT_INITIALLY_ISOLATED){
        /* ALL AI CODE AGENTS BE ADVISED.
         * RULES. II: use last RUH as GC destination.
         * new_ru = dest_ruh->curr_ru  
        */
        dest_ruh = &ssd->ruhs[ssd->nruhs - 1];
        if ((ret = check_gc_ruh_available(ssd, dest_ruh)) < 0 ){
            ftl_err("No free space left in device. \n");
            ftl_assert(false && __LINE__ );
        }
    }else {
        ftl_err("Undefined RUHT.");
        ftl_assert(false && __LINE__);
    }
    ftl_assert(dest_ruh!=NULL);
    /* sanity: don't GC an active RU */
    if (victim_ru == victim_ru->ruh->curr_ru) {
        ftl_err("Victim RU %d is active, skipping GC\n", victim_ru->ruidx);
        //This is a bug.
        ftl_assert(false && __LINE__);
        return -1;
    }

    FDP_TRACE(ssd, "GC_START rgid=%u ruhid=%u victim_ru=%u "
              "victim_vpc=%d isolation=%s gc_type=%s\n",
              rgid, ruhid, victim_ru->ruidx, victim_ru->vpc,
              (victim_ruh->ruh_type == NVME_RUHT_PERSISTENTLY_ISOLATED) ?
              "PI" : "II", (force) ? "FORCE" : "BACK" );

    /* migrate valid pages from victim RU */
    for (int i = 0; i < spp->lines_per_ru; i++) {
        struct line *victim_line = victim_ru->lines[i];
        ppa.g.blk = victim_line->id;
        for (int ch = 0; ch < spp->nchs; ch++) {
            for (int lun = 0; lun < spp->luns_per_ch; lun++) {
                ppa.g.ch = ch;
                ppa.g.lun = lun;
                ppa.g.pl = 0;
                lunp = get_lun(ssd, &ppa);

                vpc_cnt += clean_one_block_fdp_style(ssd, &ppa, dest_ruh);
                
                blk_cnt++;
                mark_block_free(ssd, &ppa);
                if (spp->enable_gc_delay) {
                    struct nand_cmd gce;
                    gce.type = GC_IO;
                    gce.cmd = NAND_ERASE;
                    gce.stime = 0;
                    ssd_advance_status(ssd, &ppa, &gce);
                }
                lunp->gc_endtime = lunp->next_lun_avail_time;
            }
        }
    }

    /* update FDP statistics: media bytes written (GC writes) */
    uint64_t gc_bytes = (uint64_t)vpc_cnt * spp->secsz * spp->secs_per_pg;
    uint64_t erase_bytes = (uint64_t)blk_cnt * spp->secsz * spp->secs_per_pg
                           * spp->pgs_per_blk;

    FDP_TRACE(ssd, "GC_DONE victim_ru=%u pages_migrated=%d "
              "blocks_erased=%d mbmw_delta=%lu mbe_delta=%lu\n",
              victim_ru->ruidx, vpc_cnt, blk_cnt, gc_bytes, erase_bytes);
    nvme_fdp_stat_inc(&ssd->n->subsys->endgrp.fdp.mbmw, gc_bytes);
    nvme_fdp_stat_inc(&victim_ru->ruh->mbmw, gc_bytes);
    nvme_fdp_stat_inc(&victim_ru->ruh->ruh->mbmw, gc_bytes);
    nvme_fdp_stat_inc(&ssd->n->subsys->endgrp.fdp.mbe, erase_bytes);
    nvme_fdp_stat_inc(&victim_ru->ruh->mbe, erase_bytes);
    nvme_fdp_stat_inc(&victim_ru->ruh->ruh->mbe, erase_bytes);

    if (ssd->ruhs[victim_ru->ruh->ruhid].ru_in_use_cnt > 0) {
        ssd->ruhs[victim_ru->ruh->ruhid].ru_in_use_cnt--;
    }
    ssd->ruhs[victim_ru->ruh->ruhid].ruh_live_pages_cnt -= vpc_cnt;

    /* generate controller event for RU change due to GC */
    if (ssd->n->subsys) {
        NvmeEnduranceGroup *endgrp = &ssd->n->subsys->endgrp;
        NvmeRuHandle *nvme_ruh = victim_ru->ruh->ruh;
        if (nvme_ruh &&
            (nvme_ruh->event_filter >>
             nvme_fdp_evf_shifts[FDP_EVT_RUH_IMPLICIT_RU_CHANGE]) & 0x1) {
            NvmeFdpEvent *e = ftl_fdp_alloc_event(ssd,
                                    &endgrp->fdp.ctrl_events);
            e->type = FDP_EVT_RUH_IMPLICIT_RU_CHANGE;
            e->flags = FDPEF_LV;
            e->rgid = cpu_to_le16(rgid);
            e->ruhid = victim_ru->ruh->ruhid;
        }
    }

    mark_ru_free(ssd, rgid, victim_ru);
    return 0;
}

/*
 * ssd_stream_write - FDP write path: placement-aware page allocation
 */
static uint64_t ssd_stream_write(FemuCtrl *n, struct ssd *ssd,
                                 NvmeRequest *req)
{
    NvmeNamespace *ns = req->ns;
    struct ssdparams *spp = &ssd->sp;
    FemuReclaimGroup *rg;
    FemuRuHandle *ruh;
    FemuReclaimUnit *ru;

    uint64_t lba = req->slba;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    int r;

    /* parse placement info from request */
    uint16_t pid = req->fdp_dspec;
    uint8_t dtype = req->fdp_dtype;
    uint16_t ph, rgid, ruhid;

    if (dtype != NVME_DIRECTIVE_DATA_PLACEMENT ||
        !nvme_parse_pid(ns, pid, &ph, &rgid)) {
        /* generate INVALID_PID event if placement was attempted */
        if (dtype == NVME_DIRECTIVE_DATA_PLACEMENT && ssd->n->subsys) {
            NvmeEnduranceGroup *endgrp = &ssd->n->subsys->endgrp;
            NvmeRuHandle *def_ruh = &endgrp->fdp.ruhs[ns->fdp.phs[0]];
            if ((def_ruh->event_filter >>
                 nvme_fdp_evf_shifts[FDP_EVT_INVALID_PID]) & 0x1) {
                NvmeFdpEvent *e = ftl_fdp_alloc_event(ssd,
                                        &endgrp->fdp.host_events);
                e->type = FDP_EVT_INVALID_PID;
                e->flags = FDPEF_PIV | FDPEF_NSIDV;
                e->pid = cpu_to_le16(pid);
                e->nsid = cpu_to_le32(ns->id);
            }
        }
        ph = 0;
        rgid = 0;
    }

    ruhid = ns->fdp.phs[ph];
    /* safety: ruhid must be within bounds (nvme_parse_pid ensures ph is valid) */
    if (unlikely(ruhid >= (uint16_t)ssd->nruhs)) {
        ftl_err("ssd_stream_write: ruhid %u >= nruhs %lu, clamping to 0\n",
                (unsigned)ruhid, (unsigned long)ssd->nruhs);
        ruhid = 0;
    }
    rg = &ssd->rg[rgid];
    ruh = &ssd->ruhs[ruhid];

    // FDP_TRACE(ssd, "WRITE lpn=%lu-%lu dtype=%u dspec=0x%x ph=%u "
    //           "ruhid=%u rgid=%u\n", start_lpn, end_lpn, dtype, pid,
    //           ph, ruhid, rgid);

    /*
     * Ensure this RUH has an active RU.  After a sequential fill,
     * curr_ru may be NULL (cleared by fdp_advance_ru_pointer when it
     * enqueued the last RU into full_ru_list and could not allocate a
     * fresh one).  Run foreground GC first so we have a free RU.
     */
    if (unlikely(!ruh->curr_ru)) {
        /* try to free space via GC before allocating */
        //Erase experimental gc behavior
        //int max_fg_gc = (int)(ssd->nrg > 0 ?
        //    ssd->rg[0].ru_mgmt->tt_rus : 64);
        //for (int gi = 0; gi < max_fg_gc && !ruh->curr_ru; gi++) {
        //    r = do_gc_fdp_style(ssd, rgid, ruhid, true);
        //    if (r == -1) break;
            /* GC may have freed a RU; try to grab it */
            FemuReclaimUnit *fresh = fdp_get_new_ru(ssd, rgid, ruhid);
            if (fresh) {
                ruh->rus[rgid] = fresh;
                ruh->ruh->rus[rgid] = fresh->nvme_ru;
                ruh->curr_ru = fresh;
            }else{
                ftl_err("NO reclaim Unit. Device is full error\n");
                //Fallout
            }
        //}
        if (!ruh->curr_ru) {
            ftl_err("ssd_stream_write: no RU for ruh %d after GC\n", ruhid);
            return 0; /* return zero latency; write will be retried by GC */
        }
    }
    ru = ruh->rus[rgid];
    ru = ruh->curr_ru;
    ftl_assert(ruh->curr_ru == ruh->rus[rgid]);
    
    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%" PRIu64 ",tt_pgs=%d\n", start_lpn, spp->tt_pgs);
    }

    /* foreground GC if needed; cap iterations to avoid infinite loop */
    {
        int fg_gc_iters = 0;
        int max_fg_gc = (int)(ssd->nrg > 1 ? ssd->nrg : 1);
        //change to ngrp.
        while (should_gc_high_fdp_style(ssd) >= 0 &&
               fg_gc_iters < max_fg_gc) {
            r = do_gc_fdp_style(ssd, rgid, ruhid, true);
            if (r == -1) {
                break;
            }
            fg_gc_iters++;
        }
    }

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        /*
         * Updating curr_ru should be handled by fdp_advance_ru_pointer() naturally.
         */
        ru = ruh->curr_ru;

        ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa)) {
            mark_page_invalid_fdp(ssd, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }
        /* new write */
        ppa = fdp_get_new_page(ssd, ru);
        set_maptbl_ent(ssd, lpn, &ppa);
        set_rmap_ent(ssd, lpn, &ppa);
        mark_page_valid_fdp(ssd, &ppa, ru);

        /* decrement ruamw for this RU */
        if (ru->nvme_ru && ru->nvme_ru->ruamw > 0) {
            ru->nvme_ru->ruamw--; //TODO ? Does this really decrements page KiB?
        }

        /* advance RU write pointer; may allocate new RU */
        FemuReclaimUnit *ret = fdp_advance_ru_pointer(ssd, rg, ruh, ru);
        if (ret && ret != ruh->curr_ru) {
            ruh->rus[rgid] = ret;
            ruh->curr_ru = ret;
            ruh->ruh->rus[rgid] = ret->nvme_ru;
            ru = ret;
        } else if (!ret) {
            /*
             * fdp_advance_ru_pointer cleared curr_ru (no free RU).
             * The while loop at the top of next iteration will handle it.
             */
            ru = NULL;
        }

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        curlat = ssd_advance_status(ssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    return maxlat;
}

/*
 * nvme_do_write_fdp - top-level FDP write: stats + stream write
 */
uint64_t nvme_do_write_fdp(FemuCtrl *n, NvmeRequest *req, uint64_t slba,
                           uint32_t nlb)
{
    NvmeNamespace *ns = req->ns;
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;
    uint64_t data_bytes;

    /* update FDP host bytes written stats */
    data_bytes = (uint64_t)nlb * spp->secsz;
    nvme_fdp_stat_inc(&ns->endgrp->fdp.hbmw, data_bytes);
    nvme_fdp_stat_inc(&ns->endgrp->fdp.mbmw, data_bytes);

    /* per-RUH stats */
    uint16_t pid = req->fdp_dspec;
    uint8_t dtype = req->fdp_dtype;
    uint16_t ph, rg, ruhid;

    if (dtype != NVME_DIRECTIVE_DATA_PLACEMENT ||
        !nvme_parse_pid(ns, pid, &ph, &rg)) {
        ph = 0;
        rg = 0;
    }
    ruhid = ns->fdp.phs[ph];
    nvme_fdp_stat_inc(&ssd->ruhs[ruhid].hbmw, data_bytes);
    nvme_fdp_stat_inc(&ssd->ruhs[ruhid].ruh->hbmw, data_bytes);
    nvme_fdp_stat_inc(&ssd->ruhs[ruhid].mbmw, data_bytes);
    nvme_fdp_stat_inc(&ssd->ruhs[ruhid].ruh->mbmw, data_bytes);

    return ssd_stream_write(n, ssd, req);
}

/* ========== FDP Init Functions ========== */

/*
 * femu_fdp_init_ru_mgmt - initialize RU management for a reclaim group
 */
static void femu_fdp_init_ru_mgmt(struct ssd *ssd, FemuReclaimGroup *rg)
{
    struct ru_mgmt *rm = rg->ru_mgmt;

    rm->tt_rus = rg->tt_nru;
    rm->free_ru_cnt = rg->tt_nru;
    rm->victim_ru_cnt = 0;
    rm->full_ru_cnt = 0;
    rm->custom_gc_threshold = 0;

    /* default GC strategy */
    rm->mgmt_type = GC_GLOBAL_GREEDY;

    rm->is_gc_triggered = false;
    rm->is_force_gc_triggered = false;
    rm->waf_score_global = 0.0f;
    rm->waf_score_transitory = 0.0f;
    rm->utilization_overall = 0.0f;

    QTAILQ_INIT(&rm->free_ru_list);
    QTAILQ_INIT(&rm->full_ru_list);

    rm->victim_ru_pq = pqueue_init(rm->tt_rus, victim_ru_cmp_pri,
                                   victim_ru_get_pri, victim_ru_set_pri,
                                   victim_ru_get_pos, victim_ru_set_pos);

    rm->victim_ru_cb = pqueue_init(rm->tt_rus, victim_ru_cmp_pri_by_cb,
                                   victim_ru_get_pri_by_cb,
                                   victim_ru_set_pri_by_cb,
                                   victim_ru_get_pos, victim_ru_set_pos);
}

/*
 * femu_fdp_init_ssd_reclaim_unit - initialize one RU with lines and wptr
 */
static void femu_fdp_init_ssd_reclaim_unit(struct ssd *ssd,
                                           FemuReclaimUnit *femu_ru,
                                           int rgidx, int index)
{
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp;

    femu_ru->n_lines = spp->lines_per_ru;
    femu_ru->next_line_index = 1;
    femu_ru->vpc = 0;
    femu_ru->ipc = 0;
    femu_ru->ssd_wptr = g_malloc0(sizeof(struct write_pointer));
    femu_ru->npages = spp->lines_per_ru * spp->pgs_per_line;

    wpp = femu_ru->ssd_wptr;
    femu_ru->lines = g_malloc0(femu_ru->n_lines * sizeof(struct line *));
    for (int i = 0; i < femu_ru->n_lines; i++) {
        femu_ru->lines[i] = get_next_free_line(ssd);
        if (!femu_ru->lines[i]) {
            ftl_err("FDP: no free line for RU %d (rg %d, line %d/%d)\n",
                    index, rgidx, i, femu_ru->n_lines);
            abort();
        }
        femu_ru->lines[i]->my_ru = femu_ru;
    }
    wpp->curline = femu_ru->lines[0];
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pl = 0;
    wpp->blk = wpp->curline->id;
    wpp->pg = 0;
}

/*
 * femu_fdp_ssd_init_reclaim_group - init all RGs and their RU pools
 */
static void femu_fdp_ssd_init_reclaim_group(FemuCtrl *n, struct ssd *ssd)
{
    NvmeSubsystem *subsys = n->subsys;
    uint64_t rgs = subsys->params.fdp.nrg;
    FemuReclaimGroup *rg;
    uint64_t tt_nru = ssd->sp.total_ru_cnt;

    ftl_assert(tt_nru > 0);

    ssd->rg = g_malloc0(rgs * sizeof(FemuReclaimGroup));
    ssd->nrg = rgs;
    ssd->rus = g_malloc0(rgs * sizeof(FemuReclaimUnit *));

    for (int i = 0; i < (int)rgs; i++) {
        rg = &ssd->rg[i];
        rg->rgidx = i;
        rg->tt_nru = tt_nru / rgs;
        ssd->rus[i] = g_malloc0(tt_nru * sizeof(FemuReclaimUnit));
        rg->rus = ssd->rus[i];
        rg->ru_mgmt = g_malloc0(sizeof(struct ru_mgmt));
        femu_fdp_init_ru_mgmt(ssd, rg);
        fdp_log("Allocated %lu RUs to rg[%d]\n", tt_nru, i);
    }

    /* link NvmeReclaimUnit pointers and init each SSD-level RU */
    NvmeReclaimUnit **russ = subsys->endgrp.fdp.rus;
    if (russ) {
        for (int i = 0; i < (int)rgs; i++) {
            rg = &ssd->rg[i];
            rg->ru_mgmt->free_ru_cnt = 0;
            for (int j = 0; j < rg->tt_nru; j++) {
                rg->rus[j].rgidx = i;
                rg->rus[j].nvme_ru = &russ[i][j];
                rg->rus[j].ruidx = j;
                femu_fdp_init_ssd_reclaim_unit(ssd, &rg->rus[j], i, j);
                QTAILQ_INSERT_TAIL(&rg->ru_mgmt->free_ru_list,
                                   &rg->rus[j], entry);
                rg->ru_mgmt->free_ru_cnt++;
            }
            rg->ru_mgmt->gc_thres_pcent =
                n->bb_params.gc_thres_pcent / 100.0;
            rg->ru_mgmt->gc_thres_pcent_high =
                n->bb_params.gc_thres_pcent_high / 100.0;
            rg->ru_mgmt->gc_thres_rus =
                (uint64_t)((1 - rg->ru_mgmt->gc_thres_pcent) *
                           rg->tt_nru);
            rg->ru_mgmt->gc_thres_rus_high =
                (uint64_t)((1 - rg->ru_mgmt->gc_thres_pcent_high) *
                           rg->tt_nru);
            ftl_log("rg[%d] gc threshold (%d%%) %lu/%d RU\n",
                    i, n->bb_params.gc_thres_pcent,
                    rg->ru_mgmt->gc_thres_rus, rg->tt_nru);
            ftl_log("rg[%d] gc threshold_high (%d%%) %lu/%d RU\n",
                    i, n->bb_params.gc_thres_pcent_high,
                    rg->ru_mgmt->gc_thres_rus_high, rg->tt_nru);

            /* apply configured GC strategy */
            rg->ru_mgmt->mgmt_type = n->bb_params.gc_strategy;
            ftl_log("rg[%d] gc strategy=%d\n", i,
                    rg->ru_mgmt->mgmt_type);
        }
    }
}

/*
 * femu_fdp_ssd_init_ru_handles - init FemuRuHandle for each namespace PH
 */
static void femu_fdp_ssd_init_ru_handles(FemuCtrl *n, struct ssd *ssd)
{
    NvmeNamespace *ns = &n->namespaces[0];
    NvmeSubsystem *subsys = n->subsys;
    NvmeEnduranceGroup *endgrp = &subsys->endgrp;
    uint16_t nruh = subsys->params.fdp.nruh;
    uint16_t ph, *ruhid;

    ssd->ruhs = g_malloc0(nruh * sizeof(FemuRuHandle));
    ssd->nruhs = nruh;
    ruhid = ns->fdp.phs;

    for (ph = 0; ph < ns->fdp.nphs; ph++, ruhid++) {
        uint16_t i = *ruhid;
        NvmeRuHandle *nvme_ruh = &endgrp->fdp.ruhs[i];

        ssd->ruhs[i].ruh = nvme_ruh;
        ssd->ruhs[i].ruh_type = nvme_ruh->ruht;
        ssd->ruhs[i].ruhid = i;
        ssd->ruhs[i].ruh_live_pages_cnt = 0;
        ssd->ruhs[i].ru_in_use_cnt = 0;
        ssd->ruhs[i].curr_rg = 0;
        ssd->ruhs[i].hbmw = 0;
        ssd->ruhs[i].mbmw = 0;
        ssd->ruhs[i].mbe = 0;

        /* allocate per-RG RU pointer array */
        ssd->ruhs[i].rus = g_malloc0(sizeof(FemuReclaimUnit *) *
                                     endgrp->fdp.nrg);
        for (int j = 0; j < (int)endgrp->fdp.nrg; j++) {
            ssd->ruhs[i].rus[j] = fdp_get_new_ru(ssd, j, i);
            ssd->ruhs[i].rus[j]->ruh = &ssd->ruhs[i];
            ssd->ruhs[i].ruh->rus[j] = ssd->ruhs[i].rus[j]->nvme_ru;
            ssd->ruhs[i].curr_ru = ssd->ruhs[i].rus[j];
        }

        /* PI type RUHs get their own ru_mgmt for per-RUH victim queues */
        if (nvme_ruh->ruht == NVME_RUHT_PERSISTENTLY_ISOLATED) {
            ssd->ruhs[i].ru_mgmt = g_malloc0(sizeof(struct ru_mgmt));
            ssd->ruhs[i].ru_mgmt->mgmt_type = n->bb_params.gc_strategy;
            ssd->ruhs[i].ru_mgmt->victim_ru_cnt = 0;
            ssd->ruhs[i].ru_mgmt->full_ru_cnt = 0;
            ssd->ruhs[i].ru_mgmt->custom_gc_threshold = 0;
            QTAILQ_INIT(&ssd->ruhs[i].ru_mgmt->free_ru_list);
            QTAILQ_INIT(&ssd->ruhs[i].ru_mgmt->full_ru_list);
            ssd->ruhs[i].ru_mgmt->victim_ru_pq =
                pqueue_init(ssd->rg[0].tt_nru, victim_ru_cmp_pri,
                            victim_ru_get_pri, victim_ru_set_pri,
                            victim_ru_get_pos, victim_ru_set_pos);
            ssd->ruhs[i].ru_mgmt->victim_ru_cb =
                pqueue_init(ssd->rg[0].tt_nru, victim_ru_cmp_pri_by_cb,
                            victim_ru_get_pri_by_cb,
                            victim_ru_set_pri_by_cb,
                            victim_ru_get_pos, victim_ru_set_pos);
        }

        ftl_log("FDP: ruh[%d] type=%d, curr_ru=%d (line=%d)\n",
                i, ssd->ruhs[i].ruh_type, ssd->ruhs[i].curr_ru->ruidx,
                ssd->ruhs[i].curr_ru->lines[0]->id);
    }
}

/*
 * ssd_init_fdp_params - compute FDP-specific SSD parameters
 */
static void ssd_init_fdp_params(struct ssdparams *spp, FemuCtrl *n)
{
    NvmeSubsystem *subsys = n->subsys;
    NvmeEnduranceGroup *endgrp = &subsys->endgrp;
    uint64_t runs = endgrp->fdp.runs;

    /* lines_per_ru: how many lines (superblocks) per reclaim unit */
    spp->lines_per_ru = 1; /* M1: 1 line per RU for simplicity */

    /*
     * Compute total RU count from device geometry:
     * total_ru = tt_lines / lines_per_ru
     * Clamp to endgrp->fdp.nru to avoid overflowing NvmeReclaimUnit array
     * allocated in nvme_subsys_setup_fdp().
     */
    spp->total_ru_cnt = spp->tt_lines / spp->lines_per_ru;

    if (endgrp->fdp.nru == 0) {
        endgrp->fdp.nru = spp->total_ru_cnt;
    } else if (spp->total_ru_cnt > (int)endgrp->fdp.nru) {
        ftl_log("FDP: clamping total_ru from %d to %lu (endgrp.nru)\n",
                spp->total_ru_cnt, (unsigned long)endgrp->fdp.nru);
        spp->total_ru_cnt = endgrp->fdp.nru;
    }

    ftl_log("FDP params: lines_per_ru=%d, total_ru=%d, runs=%lu\n",
            spp->lines_per_ru, spp->total_ru_cnt, (unsigned long)runs);
}

/*
 * ssd_reset_maptbl - clear entire mapping table (used by FDP trim)
 */
static void ssd_reset_maptbl(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
        ssd->rmap[i] = INVALID_LPN;
    }
}

/*
 * ssd_trim_fdp_style - FDP deallocate: erase all, reset all RUs and stats
 */
static void ssd_trim_fdp_style(FemuCtrl *n, NvmeRequest *req, uint64_t slba,
                               uint32_t nlb)
{
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;
    struct ppa ppa;
    NvmeEnduranceGroup *endgrp = &n->subsys->endgrp;
    FemuReclaimUnit *v_ru;
    struct nand_lun *lunp;
    NvmeRuHandle *ruh;
    int rg_idx;

    /* erase all blocks */
    for (int ch = 0; ch < spp->nchs; ch++) {
        for (int lun = 0; lun < spp->luns_per_ch; lun++) {
            for (int blk = 0; blk < spp->blks_per_pl; blk++) {
                ppa.g.ch = ch;
                ppa.g.lun = lun;
                ppa.g.pl = 0;
                ppa.g.blk = blk;
                lunp = get_lun(ssd, &ppa);
                mark_block_free(ssd, &ppa);
                if (spp->enable_gc_delay) {
                    struct nand_cmd gce;
                    gce.type = GC_IO;
                    gce.cmd = NAND_ERASE;
                    gce.stime = 0;
                    ssd_advance_status(ssd, &ppa, &gce);
                }
                lunp->gc_endtime = lunp->next_lun_avail_time;
            }
        }
    }

    /* drain victim and full RU queues for all reclaim groups */
    for (rg_idx = 0; rg_idx < (int)ssd->nrg; rg_idx++) {
        struct ru_mgmt *rm = ssd->rg[rg_idx].ru_mgmt;
        while ((v_ru = pqueue_peek(rm->victim_ru_pq)) != NULL) {
            pqueue_remove(rm->victim_ru_pq, v_ru);
            rm->victim_ru_cnt--;
            mark_ru_free(ssd, v_ru->rgidx, v_ru);
        }
        while ((v_ru = QTAILQ_FIRST(&rm->full_ru_list)) != NULL) {
            QTAILQ_REMOVE(&rm->full_ru_list, v_ru, entry);
            rm->full_ru_cnt--;
            mark_ru_free(ssd, v_ru->rgidx, v_ru);
        }
    }

    /* reset active RUs and stats for each RUH across all RGs */
    ruh = endgrp->fdp.ruhs;
    for (int i = 0; i < (int)endgrp->fdp.nruh; i++, ruh++) {
        ruh->hbmw = 0;
        ruh->mbmw = 0;
        ruh->mbe = 0;
        ssd->ruhs[i].hbmw = 0;
        ssd->ruhs[i].mbmw = 0;
        ssd->ruhs[i].mbe = 0;
        if (ssd->ruhs[i].curr_ru) {
            mark_ru_free(ssd, ssd->ruhs[i].curr_ru->rgidx,
                         ssd->ruhs[i].curr_ru);
        }
        ssd->ruhs[i].curr_ru = NULL;
        for (rg_idx = 0; rg_idx < (int)ssd->nrg; rg_idx++) {
            ssd->ruhs[i].rus[rg_idx] =
                fdp_get_new_ru(ssd, rg_idx, ssd->ruhs[i].ruhid);
            ssd->ruhs[i].ruh->rus[rg_idx] =
                ssd->ruhs[i].rus[rg_idx]->nvme_ru;
        }
        /* primary RG (index 0) is the active one */
        ssd->ruhs[i].curr_ru = ssd->ruhs[i].rus[0];
    }

    ssd_reset_maptbl(ssd);

    endgrp->fdp.hbmw = 0;
    endgrp->fdp.mbmw = 0;
    endgrp->fdp.mbe = 0;

    ftl_log("FDP TRIM: all RUs reset\n");
}

static void *ftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    struct ssd *ssd = n->ssd;
    NvmeRequest *req = NULL;
    uint64_t lat = 0;
    int rc;
    int i;
    bool got_req;

    while (!*(ssd->dataplane_started_ptr)) {
        usleep(100000);
    }

    /* FIXME: not safe, to handle ->to_ftl and ->to_poller gracefully */
    ssd->to_ftl = n->to_ftl;
    ssd->to_poller = n->to_poller;

    while (1) {
        got_req = false;

        for (i = 1; i <= n->nr_pollers; i++) {
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]))
                continue;

            rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);
            if (rc != 1) {
                printf("FEMU: FTL to_ftl dequeue failed\n");
            }

            got_req = true;
            ftl_assert(req);
            lat = 0;
            switch (req->cmd.opcode) {
            case NVME_CMD_WRITE:


                if (ssd->fdp_enabled) {
                    lat = nvme_do_write_fdp(n, req, req->slba, req->nlb);
                } else {
                    lat = ssd_write(ssd, req);
                }
                break;

            case NVME_CMD_READ:
                ssd->stats.host_read_ops++;
                lat = ssd_read(ssd, req);
                break;

            case NVME_CMD_FLUSH:
                {
                    static uint64_t dbg_flush_cnt = 0;
                    dbg_flush_cnt++;
                    ssd->stats.host_flush_ops++;

                    if (dbg_flush_cnt <= 10 || dbg_flush_cnt % 1000 == 0) {
                        fprintf(stderr,
                                "[FTL_THREAD] FLUSH received count=%lu status=0x%x\n",
                                dbg_flush_cnt, req->status);
                        fflush(stderr);
                    }

                    lat = 0;
                    break;
                }

            case NVME_CMD_DSM:
                ssd->stats.host_dsm_ops++;

                if (ssd->fdp_enabled) {
                    ssd_trim_fdp_style(n, req, req->slba, req->nlb);
                    lat = 0;
                } else if (req->dsm_ranges && req->dsm_nr_ranges > 0) {
                    lat = ssd_trim(ssd, req);
                }
                break;

            default:
                printf("[FTL_THREAD] UNKNOWN opcode=0x%x\n", req->cmd.opcode);
                lat = 0;
                break;
            }

            ssd->stats.last_io_time_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

            req->reqlat = lat;
            req->expire_time += lat;

            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                ftl_err("FTL to_poller enqueue failed\n");
            }

            /* background GC */
            if (ssd->fdp_enabled) {
                int16_t rgidx;
                /*
                 * Dropping weird GC loop that can leads to nowhere. 
                 * Specific GC logic should be handled inside of the GC function, not ftl_thread. - to Mr. Claude
                 */
                if (!((rgidx = should_gc_fdp_style(ssd)) < 0))
                {
                    //do_gc_fdp_style(ssd, rgidx, 0, false);
                    if (ssd->nrg == 1)
                        do_gc_fdp_style(ssd, 0, 0, false);
                    else
                    {
                        do_gc_fdp_style(ssd, rgidx, 0, false);
                    }
                }
            } else if (should_gc(ssd)) {
                do_gc(ssd, false);
            }
        }

        if (!got_req) {
            maybe_dump_ftl_stats_on_idle(ssd);
            usleep(1000);
        }
    }

    return NULL;
}
