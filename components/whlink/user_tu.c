/* A separate TU that only includes declarations and uses the API — proves the
 * stb-style one-impl/many-users split links with no missing/duplicate symbols. */
#include "wh_link.h"
#include <string.h>
#include <stdio.h>
static int ts(void *u, const uint8_t *f, size_t n){ (void)u;(void)f;(void)n; return 0; }
static void rng(void *u, uint8_t *b, size_t n){ (void)u; for (size_t i=0;i<n;i++) b[i]=(uint8_t)(i*7+1); }
int main(void){
    wh_ctx c; wh_config cfg; memset(&cfg,0,sizeof(cfg));
    cfg.local_id=1; cfg.peer_id=2; cfg.role=WH_ROLE_RING; cfg.caps=WH_CAPS_RING;
    cfg.rng=rng; cfg.transport_send=ts;
    int r = wh_ctx_init(&c,&cfg);
    printf("split init=%d (%s) hdr_len=%d cap_enc=%d\n", r, wh_strerr(r), WH_HDR_LEN, WH_FRAG_CAP_ENC);
    return r==WH_OK ? 0 : 1;
}
