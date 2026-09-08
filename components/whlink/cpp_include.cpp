/* Proves wh_link.h is includable from a C++ TU and links against the C impl. */
#include "wh_link.h"
#include <cstring>
#include <cstdio>
static int ts(void *u, const uint8_t *f, size_t n){ (void)u;(void)f;(void)n; return 0; }
static void rng(void *u, uint8_t *b, size_t n){ (void)u; for (size_t i=0;i<n;i++) b[i]=(uint8_t)i; }
int main(){
    wh_ctx c; wh_config cfg; std::memset(&cfg,0,sizeof(cfg));
    cfg.local_id=2; cfg.peer_id=1; cfg.role=WH_ROLE_WATCH; cfg.caps=WH_CAPS_WATCH;
    cfg.rng=rng; cfg.transport_send=ts;
    int r = wh_ctx_init(&c,&cfg);
    std::printf("cpp init=%d (%s)\n", r, wh_strerr(r));
    return r==WH_OK ? 0 : 1;
}
