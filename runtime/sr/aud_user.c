// aud:u, the Switch 2 SDK's audio client service (nn::audio::CreateUserClientSessionByHipc). Its sub-objects are the
// interfaces totk-pc-recomp's runtime already serves under their own names (audren.c); this handler hands them out.
#include <stdint.h>

#include "../core/audren.h"
#include "../core/host.h"
#include "../core/ipc.h"

void aud_user_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 2:   // the audio renderer manager (from nn::audio::CreateAudioRendererManager; 8 bytes in, read as unused)
        rs->move[rs->nmove++] = ipc_session_new("IAudioRendererManager", audren_manager_handler, NULL, NULL);
        return;
    default: ipc_unimplemented(s, rq);
    }
}
