/* agent/shared/audiofix.h - a sound card with a driver and no wave device
 * (agent 1.85.2, gamesync.c gs_audio_stack_check).
 *
 * 2026-09-26, Dell Dimension 4600: agent 1.85.1 installed the SoundMAX driver
 * ("installed - device working"), and waveOutGetNumDevs() stayed 0 across a
 * reboot. wdmaudio.inf's RunOnce registrations for sysaudio/kmixer/wdmaud had
 * been consumed without running. Before 1.85.2 nothing looked (the "old"
 * verdict: never repair); now the agent re-runs the box's own registration. */
#include <stdio.h>
#include "../../agent/shared/audiofix.h"

static int fails = 0, runs = 0;
#define CHECK(c, msg) do { runs++; if (!(c)) { fails++; printf("  [FAIL] %s\n", msg); } else printf("  [ ok ] %s\n", msg); } while (0)

static int old_verdict(void) { return AUDIOFIX_NONE; }

int main(void)
{
    CHECK(audiofix_decide(1, 0, 1, 0, 0) == AUDIOFIX_REPAIR,
          "the Dell: XP, 0 wave devices, SoundMAX bound, sysaudio unregistered -> REPAIR");
    CHECK(old_verdict() == AUDIOFIX_NONE, "(1.85.1 never looked - no sound, nothing said)");
    CHECK(audiofix_decide(1, 1, 1, 0, 0) == AUDIOFIX_NONE,
          "a wave device exists -> nothing to do (the every-boot case costs one call)");
    CHECK(audiofix_decide(1, 0, 0, 0, 0) == AUDIOFIX_NONE,
          "no sound hardware carries a driver -> nothing to do");
    CHECK(audiofix_decide(0, 0, 1, 0, 0) == AUDIOFIX_NONE,
          "Win9x -> never (a different audio stack)");
    CHECK(audiofix_decide(1, 0, 1, 1, 0) == AUDIOFIX_OTHER_FAULT,
          "sysaudio IS registered -> a different fault, reported, not 'repaired'");
    CHECK(audiofix_decide(1, 0, 1, 0, AUDIOFIX_MAX_ATTEMPTS) == AUDIOFIX_GAVE_UP,
          "attempts exhausted -> stop, and say NO SOUND");
    CHECK(audiofix_decide(1, 0, 1, 0, AUDIOFIX_MAX_ATTEMPTS - 1) == AUDIOFIX_REPAIR,
          "one attempt left -> repair");

    CHECK(audiofix_is_hw_id("pci\\ven_8086&dev_24d5&subsys_01741028"), "the SoundMAX id is hardware");
    CHECK(audiofix_is_hw_id("ISAPNP\\CTL0045"), "an ISA PnP AWE64 is hardware");
    CHECK(!audiofix_is_hw_id("ms_mmdrv") && !audiofix_is_hw_id("MS_MMMCI"),
          "XP's own ms_* media entries are not");
    CHECK(!audiofix_is_hw_id("sw\\{a7c7a5b0-5af3-11d1-9ced-00a024bf0407}"),
          "the kernel-audio software devices are not");
    CHECK(!audiofix_is_hw_id("root\\legacy_foo") && !audiofix_is_hw_id(""),
          "root-enumerated and empty ids are not");

    CHECK(audiofix_is_streamci_cmd("rundll32.exe streamci.dll,StreamingDeviceSetup {A7C7A5B0-5AF3-11D1-9CED-00A024BF0407},{9B365890-165F-11D0-A195-0020AFD156E4},{A7C7A5B1-5AF3-11D1-9CED-00A024BF0407},C:\\WINDOWS\\inf\\WDMAUDIO.inf,WDM_SYSAUDIO.Interface.Install"),
          "wdmaudio's sysaudio registration is one the repair may run");
    CHECK(audiofix_is_streamci_cmd("rundll32.exe streamci,STREAMINGDEVICESETUP x"),
          "case-insensitively, including the .dll-less form wdmaudio uses for DRMKAUD");
    CHECK(!audiofix_is_streamci_cmd("C:\\Program Files\\Vendor\\setup.exe /finish") &&
          !audiofix_is_streamci_cmd(NULL),
          "any other RunOnce entry is left for Windows");
    printf("-- audiofix (sound card with no wave device, agent 1.85.2): %d/%d tests passed --\n",
           runs - fails, runs);
    return fails ? 1 : 0;
}
