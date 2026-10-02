#define WINMM_EXPORT_COUNT 209
static const char* const g_fwdNames[WINMM_EXPORT_COUNT] = {
"CloseDriver",
"DefDriverProc",
"DriverCallback",
"DrvGetModuleHandle",
"GetDriverModuleHandle",
"MigrateAllDrivers",
"MigrateSoundEvents",
"NotifyCallbackData",
"OpenDriver",
"PlaySound",
"PlaySoundA",
"PlaySoundW",
"SendDriverMessage",
"WOW32DriverCallback",
"WOW32ResolveMultiMediaHandle",
"WOWAppExit",
"WinmmLogoff",
"WinmmLogon",
"aux32Message",
"auxGetDevCapsA",
"auxGetDevCapsW",
"auxGetNumDevs",
"auxGetVolume",
"auxOutMessage",
"auxSetVolume",
"gfxAddGfx",
"gfxBatchChange",
"gfxCreateGfxFactoriesList",
"gfxCreateZoneFactoriesList",
"gfxDestroyDeviceInterfaceList",
"gfxEnumerateGfxs",
"gfxLogoff",
"gfxLogon",
"gfxModifyGfx",
"gfxOpenGfx",
"gfxRemoveGfx",
"joy32Message",
"joyConfigChanged",
"joyGetDevCapsA",
"joyGetDevCapsW",
"joyGetNumDevs",
"joyGetPos",
"joyGetPosEx",
"joyGetThreshold",
"joyReleaseCapture",
"joySetCapture",
"joySetThreshold",
"mci32Message",
"mciDriverNotify",
"mciDriverYield",
"mciExecute",
"mciFreeCommandResource",
"mciGetCreatorTask",
"mciGetDeviceIDA",
"mciGetDeviceIDFromElementIDA",
"mciGetDeviceIDFromElementIDW",
"mciGetDeviceIDW",
"mciGetDriverData",
"mciGetErrorStringA",
"mciGetErrorStringW",
"mciGetYieldProc",
"mciLoadCommandResource",
"mciSendCommandA",
"mciSendCommandW",
"mciSendStringA",
"mciSendStringW",
"mciSetDriverData",
"mciSetYieldProc",
"mid32Message",
"midiConnect",
"midiDisconnect",
"midiInAddBuffer",
"midiInClose",
"midiInGetDevCapsA",
"midiInGetDevCapsW",
"midiInGetErrorTextA",
"midiInGetErrorTextW",
"midiInGetID",
"midiInGetNumDevs",
"midiInMessage",
"midiInOpen",
"midiInPrepareHeader",
"midiInReset",
"midiInStart",
"midiInStop",
"midiInUnprepareHeader",
"midiOutCacheDrumPatches",
"midiOutCachePatches",
"midiOutClose",
"midiOutGetDevCapsA",
"midiOutGetDevCapsW",
"midiOutGetErrorTextA",
"midiOutGetErrorTextW",
"midiOutGetID",
"midiOutGetNumDevs",
"midiOutGetVolume",
"midiOutLongMsg",
"midiOutMessage",
"midiOutOpen",
"midiOutPrepareHeader",
"midiOutReset",
"midiOutSetVolume",
"midiOutShortMsg",
"midiOutUnprepareHeader",
"midiStreamClose",
"midiStreamOpen",
"midiStreamOut",
"midiStreamPause",
"midiStreamPosition",
"midiStreamProperty",
"midiStreamRestart",
"midiStreamStop",
"mixerClose",
"mixerGetControlDetailsA",
"mixerGetControlDetailsW",
"mixerGetDevCapsA",
"mixerGetDevCapsW",
"mixerGetID",
"mixerGetLineControlsA",
"mixerGetLineControlsW",
"mixerGetLineInfoA",
"mixerGetLineInfoW",
"mixerGetNumDevs",
"mixerMessage",
"mixerOpen",
"mixerSetControlDetails",
"mmDrvInstall",
"mmGetCurrentTask",
"mmTaskBlock",
"mmTaskCreate",
"mmTaskSignal",
"mmTaskYield",
"mmioAdvance",
"mmioAscend",
"mmioClose",
"mmioCreateChunk",
"mmioDescend",
"mmioFlush",
"mmioGetInfo",
"mmioInstallIOProcA",
"mmioInstallIOProcW",
"mmioOpenA",
"mmioOpenW",
"mmioRead",
"mmioRenameA",
"mmioRenameW",
"mmioSeek",
"mmioSendMessage",
"mmioSetBuffer",
"mmioSetInfo",
"mmioStringToFOURCCA",
"mmioStringToFOURCCW",
"mmioWrite",
"mmsystemGetVersion",
"mod32Message",
"mxd32Message",
"sndPlaySoundA",
"sndPlaySoundW",
"tid32Message",
"timeBeginPeriod",
"timeEndPeriod",
"timeGetDevCaps",
"timeGetSystemTime",
"timeGetTime",
"timeKillEvent",
"timeSetEvent",
"waveInAddBuffer",
"waveInClose",
"waveInGetDevCapsA",
"waveInGetDevCapsW",
"waveInGetErrorTextA",
"waveInGetErrorTextW",
"waveInGetID",
"waveInGetNumDevs",
"waveInGetPosition",
"waveInMessage",
"waveInOpen",
"waveInPrepareHeader",
"waveInReset",
"waveInStart",
"waveInStop",
"waveInUnprepareHeader",
"waveOutBreakLoop",
"waveOutClose",
"waveOutGetDevCapsA",
"waveOutGetDevCapsW",
"waveOutGetErrorTextA",
"waveOutGetErrorTextW",
"waveOutGetID",
"waveOutGetNumDevs",
"waveOutGetPitch",
"waveOutGetPlaybackRate",
"waveOutGetPosition",
"waveOutGetVolume",
"waveOutMessage",
"waveOutOpen",
"waveOutPause",
"waveOutPrepareHeader",
"waveOutReset",
"waveOutRestart",
"waveOutSetPitch",
"waveOutSetPlaybackRate",
"waveOutSetVolume",
"waveOutUnprepareHeader",
"waveOutWrite",
"wid32Message",
"winmmDbgOut",
"winmmSetDebugLevel",
"wod32Message",
};
__asm__(".text\n"
".globl CloseDriver\nCloseDriver:\n jmp *g_fwd+0(%rip)\n"
".globl DefDriverProc\nDefDriverProc:\n jmp *g_fwd+8(%rip)\n"
".globl DriverCallback\nDriverCallback:\n jmp *g_fwd+16(%rip)\n"
".globl DrvGetModuleHandle\nDrvGetModuleHandle:\n jmp *g_fwd+24(%rip)\n"
".globl GetDriverModuleHandle\nGetDriverModuleHandle:\n jmp *g_fwd+32(%rip)\n"
".globl MigrateAllDrivers\nMigrateAllDrivers:\n jmp *g_fwd+40(%rip)\n"
".globl MigrateSoundEvents\nMigrateSoundEvents:\n jmp *g_fwd+48(%rip)\n"
".globl NotifyCallbackData\nNotifyCallbackData:\n jmp *g_fwd+56(%rip)\n"
".globl OpenDriver\nOpenDriver:\n jmp *g_fwd+64(%rip)\n"
".globl PlaySound\nPlaySound:\n jmp *g_fwd+72(%rip)\n"
".globl PlaySoundA\nPlaySoundA:\n jmp *g_fwd+80(%rip)\n"
".globl PlaySoundW\nPlaySoundW:\n jmp *g_fwd+88(%rip)\n"
".globl SendDriverMessage\nSendDriverMessage:\n jmp *g_fwd+96(%rip)\n"
".globl WOW32DriverCallback\nWOW32DriverCallback:\n jmp *g_fwd+104(%rip)\n"
".globl WOW32ResolveMultiMediaHandle\nWOW32ResolveMultiMediaHandle:\n jmp *g_fwd+112(%rip)\n"
".globl WOWAppExit\nWOWAppExit:\n jmp *g_fwd+120(%rip)\n"
".globl WinmmLogoff\nWinmmLogoff:\n jmp *g_fwd+128(%rip)\n"
".globl WinmmLogon\nWinmmLogon:\n jmp *g_fwd+136(%rip)\n"
".globl aux32Message\naux32Message:\n jmp *g_fwd+144(%rip)\n"
".globl auxGetDevCapsA\nauxGetDevCapsA:\n jmp *g_fwd+152(%rip)\n"
".globl auxGetDevCapsW\nauxGetDevCapsW:\n jmp *g_fwd+160(%rip)\n"
".globl auxGetNumDevs\nauxGetNumDevs:\n jmp *g_fwd+168(%rip)\n"
".globl auxGetVolume\nauxGetVolume:\n jmp *g_fwd+176(%rip)\n"
".globl auxOutMessage\nauxOutMessage:\n jmp *g_fwd+184(%rip)\n"
".globl auxSetVolume\nauxSetVolume:\n jmp *g_fwd+192(%rip)\n"
".globl gfxAddGfx\ngfxAddGfx:\n jmp *g_fwd+200(%rip)\n"
".globl gfxBatchChange\ngfxBatchChange:\n jmp *g_fwd+208(%rip)\n"
".globl gfxCreateGfxFactoriesList\ngfxCreateGfxFactoriesList:\n jmp *g_fwd+216(%rip)\n"
".globl gfxCreateZoneFactoriesList\ngfxCreateZoneFactoriesList:\n jmp *g_fwd+224(%rip)\n"
".globl gfxDestroyDeviceInterfaceList\ngfxDestroyDeviceInterfaceList:\n jmp *g_fwd+232(%rip)\n"
".globl gfxEnumerateGfxs\ngfxEnumerateGfxs:\n jmp *g_fwd+240(%rip)\n"
".globl gfxLogoff\ngfxLogoff:\n jmp *g_fwd+248(%rip)\n"
".globl gfxLogon\ngfxLogon:\n jmp *g_fwd+256(%rip)\n"
".globl gfxModifyGfx\ngfxModifyGfx:\n jmp *g_fwd+264(%rip)\n"
".globl gfxOpenGfx\ngfxOpenGfx:\n jmp *g_fwd+272(%rip)\n"
".globl gfxRemoveGfx\ngfxRemoveGfx:\n jmp *g_fwd+280(%rip)\n"
".globl joy32Message\njoy32Message:\n jmp *g_fwd+288(%rip)\n"
".globl joyConfigChanged\njoyConfigChanged:\n jmp *g_fwd+296(%rip)\n"
".globl joyGetDevCapsA\njoyGetDevCapsA:\n jmp *g_fwd+304(%rip)\n"
".globl joyGetDevCapsW\njoyGetDevCapsW:\n jmp *g_fwd+312(%rip)\n"
".globl joyGetNumDevs\njoyGetNumDevs:\n jmp *g_fwd+320(%rip)\n"
".globl joyGetPos\njoyGetPos:\n jmp *g_fwd+328(%rip)\n"
".globl joyGetPosEx\njoyGetPosEx:\n jmp *g_fwd+336(%rip)\n"
".globl joyGetThreshold\njoyGetThreshold:\n jmp *g_fwd+344(%rip)\n"
".globl joyReleaseCapture\njoyReleaseCapture:\n jmp *g_fwd+352(%rip)\n"
".globl joySetCapture\njoySetCapture:\n jmp *g_fwd+360(%rip)\n"
".globl joySetThreshold\njoySetThreshold:\n jmp *g_fwd+368(%rip)\n"
".globl mci32Message\nmci32Message:\n jmp *g_fwd+376(%rip)\n"
".globl mciDriverNotify\nmciDriverNotify:\n jmp *g_fwd+384(%rip)\n"
".globl mciDriverYield\nmciDriverYield:\n jmp *g_fwd+392(%rip)\n"
".globl mciExecute\nmciExecute:\n jmp *g_fwd+400(%rip)\n"
".globl mciFreeCommandResource\nmciFreeCommandResource:\n jmp *g_fwd+408(%rip)\n"
".globl mciGetCreatorTask\nmciGetCreatorTask:\n jmp *g_fwd+416(%rip)\n"
".globl mciGetDeviceIDA\nmciGetDeviceIDA:\n jmp *g_fwd+424(%rip)\n"
".globl mciGetDeviceIDFromElementIDA\nmciGetDeviceIDFromElementIDA:\n jmp *g_fwd+432(%rip)\n"
".globl mciGetDeviceIDFromElementIDW\nmciGetDeviceIDFromElementIDW:\n jmp *g_fwd+440(%rip)\n"
".globl mciGetDeviceIDW\nmciGetDeviceIDW:\n jmp *g_fwd+448(%rip)\n"
".globl mciGetDriverData\nmciGetDriverData:\n jmp *g_fwd+456(%rip)\n"
".globl mciGetErrorStringA\nmciGetErrorStringA:\n jmp *g_fwd+464(%rip)\n"
".globl mciGetErrorStringW\nmciGetErrorStringW:\n jmp *g_fwd+472(%rip)\n"
".globl mciGetYieldProc\nmciGetYieldProc:\n jmp *g_fwd+480(%rip)\n"
".globl mciLoadCommandResource\nmciLoadCommandResource:\n jmp *g_fwd+488(%rip)\n"
".globl mciSendCommandA\nmciSendCommandA:\n jmp *g_fwd+496(%rip)\n"
".globl mciSendCommandW\nmciSendCommandW:\n jmp *g_fwd+504(%rip)\n"
".globl mciSendStringA\nmciSendStringA:\n jmp *g_fwd+512(%rip)\n"
".globl mciSendStringW\nmciSendStringW:\n jmp *g_fwd+520(%rip)\n"
".globl mciSetDriverData\nmciSetDriverData:\n jmp *g_fwd+528(%rip)\n"
".globl mciSetYieldProc\nmciSetYieldProc:\n jmp *g_fwd+536(%rip)\n"
".globl mid32Message\nmid32Message:\n jmp *g_fwd+544(%rip)\n"
".globl midiConnect\nmidiConnect:\n jmp *g_fwd+552(%rip)\n"
".globl midiDisconnect\nmidiDisconnect:\n jmp *g_fwd+560(%rip)\n"
".globl midiInAddBuffer\nmidiInAddBuffer:\n jmp *g_fwd+568(%rip)\n"
".globl midiInClose\nmidiInClose:\n jmp *g_fwd+576(%rip)\n"
".globl midiInGetDevCapsA\nmidiInGetDevCapsA:\n jmp *g_fwd+584(%rip)\n"
".globl midiInGetDevCapsW\nmidiInGetDevCapsW:\n jmp *g_fwd+592(%rip)\n"
".globl midiInGetErrorTextA\nmidiInGetErrorTextA:\n jmp *g_fwd+600(%rip)\n"
".globl midiInGetErrorTextW\nmidiInGetErrorTextW:\n jmp *g_fwd+608(%rip)\n"
".globl midiInGetID\nmidiInGetID:\n jmp *g_fwd+616(%rip)\n"
".globl midiInGetNumDevs\nmidiInGetNumDevs:\n jmp *g_fwd+624(%rip)\n"
".globl midiInMessage\nmidiInMessage:\n jmp *g_fwd+632(%rip)\n"
".globl midiInOpen\nmidiInOpen:\n jmp *g_fwd+640(%rip)\n"
".globl midiInPrepareHeader\nmidiInPrepareHeader:\n jmp *g_fwd+648(%rip)\n"
".globl midiInReset\nmidiInReset:\n jmp *g_fwd+656(%rip)\n"
".globl midiInStart\nmidiInStart:\n jmp *g_fwd+664(%rip)\n"
".globl midiInStop\nmidiInStop:\n jmp *g_fwd+672(%rip)\n"
".globl midiInUnprepareHeader\nmidiInUnprepareHeader:\n jmp *g_fwd+680(%rip)\n"
".globl midiOutCacheDrumPatches\nmidiOutCacheDrumPatches:\n jmp *g_fwd+688(%rip)\n"
".globl midiOutCachePatches\nmidiOutCachePatches:\n jmp *g_fwd+696(%rip)\n"
".globl midiOutClose\nmidiOutClose:\n jmp *g_fwd+704(%rip)\n"
".globl midiOutGetDevCapsA\nmidiOutGetDevCapsA:\n jmp *g_fwd+712(%rip)\n"
".globl midiOutGetDevCapsW\nmidiOutGetDevCapsW:\n jmp *g_fwd+720(%rip)\n"
".globl midiOutGetErrorTextA\nmidiOutGetErrorTextA:\n jmp *g_fwd+728(%rip)\n"
".globl midiOutGetErrorTextW\nmidiOutGetErrorTextW:\n jmp *g_fwd+736(%rip)\n"
".globl midiOutGetID\nmidiOutGetID:\n jmp *g_fwd+744(%rip)\n"
".globl midiOutGetNumDevs\nmidiOutGetNumDevs:\n jmp *g_fwd+752(%rip)\n"
".globl midiOutGetVolume\nmidiOutGetVolume:\n jmp *g_fwd+760(%rip)\n"
".globl midiOutLongMsg\nmidiOutLongMsg:\n jmp *g_fwd+768(%rip)\n"
".globl midiOutMessage\nmidiOutMessage:\n jmp *g_fwd+776(%rip)\n"
".globl midiOutOpen\nmidiOutOpen:\n jmp *g_fwd+784(%rip)\n"
".globl midiOutPrepareHeader\nmidiOutPrepareHeader:\n jmp *g_fwd+792(%rip)\n"
".globl midiOutReset\nmidiOutReset:\n jmp *g_fwd+800(%rip)\n"
".globl midiOutSetVolume\nmidiOutSetVolume:\n jmp *g_fwd+808(%rip)\n"
".globl midiOutShortMsg\nmidiOutShortMsg:\n jmp *g_fwd+816(%rip)\n"
".globl midiOutUnprepareHeader\nmidiOutUnprepareHeader:\n jmp *g_fwd+824(%rip)\n"
".globl midiStreamClose\nmidiStreamClose:\n jmp *g_fwd+832(%rip)\n"
".globl midiStreamOpen\nmidiStreamOpen:\n jmp *g_fwd+840(%rip)\n"
".globl midiStreamOut\nmidiStreamOut:\n jmp *g_fwd+848(%rip)\n"
".globl midiStreamPause\nmidiStreamPause:\n jmp *g_fwd+856(%rip)\n"
".globl midiStreamPosition\nmidiStreamPosition:\n jmp *g_fwd+864(%rip)\n"
".globl midiStreamProperty\nmidiStreamProperty:\n jmp *g_fwd+872(%rip)\n"
".globl midiStreamRestart\nmidiStreamRestart:\n jmp *g_fwd+880(%rip)\n"
".globl midiStreamStop\nmidiStreamStop:\n jmp *g_fwd+888(%rip)\n"
".globl mixerClose\nmixerClose:\n jmp *g_fwd+896(%rip)\n"
".globl mixerGetControlDetailsA\nmixerGetControlDetailsA:\n jmp *g_fwd+904(%rip)\n"
".globl mixerGetControlDetailsW\nmixerGetControlDetailsW:\n jmp *g_fwd+912(%rip)\n"
".globl mixerGetDevCapsA\nmixerGetDevCapsA:\n jmp *g_fwd+920(%rip)\n"
".globl mixerGetDevCapsW\nmixerGetDevCapsW:\n jmp *g_fwd+928(%rip)\n"
".globl mixerGetID\nmixerGetID:\n jmp *g_fwd+936(%rip)\n"
".globl mixerGetLineControlsA\nmixerGetLineControlsA:\n jmp *g_fwd+944(%rip)\n"
".globl mixerGetLineControlsW\nmixerGetLineControlsW:\n jmp *g_fwd+952(%rip)\n"
".globl mixerGetLineInfoA\nmixerGetLineInfoA:\n jmp *g_fwd+960(%rip)\n"
".globl mixerGetLineInfoW\nmixerGetLineInfoW:\n jmp *g_fwd+968(%rip)\n"
".globl mixerGetNumDevs\nmixerGetNumDevs:\n jmp *g_fwd+976(%rip)\n"
".globl mixerMessage\nmixerMessage:\n jmp *g_fwd+984(%rip)\n"
".globl mixerOpen\nmixerOpen:\n jmp *g_fwd+992(%rip)\n"
".globl mixerSetControlDetails\nmixerSetControlDetails:\n jmp *g_fwd+1000(%rip)\n"
".globl mmDrvInstall\nmmDrvInstall:\n jmp *g_fwd+1008(%rip)\n"
".globl mmGetCurrentTask\nmmGetCurrentTask:\n jmp *g_fwd+1016(%rip)\n"
".globl mmTaskBlock\nmmTaskBlock:\n jmp *g_fwd+1024(%rip)\n"
".globl mmTaskCreate\nmmTaskCreate:\n jmp *g_fwd+1032(%rip)\n"
".globl mmTaskSignal\nmmTaskSignal:\n jmp *g_fwd+1040(%rip)\n"
".globl mmTaskYield\nmmTaskYield:\n jmp *g_fwd+1048(%rip)\n"
".globl mmioAdvance\nmmioAdvance:\n jmp *g_fwd+1056(%rip)\n"
".globl mmioAscend\nmmioAscend:\n jmp *g_fwd+1064(%rip)\n"
".globl mmioClose\nmmioClose:\n jmp *g_fwd+1072(%rip)\n"
".globl mmioCreateChunk\nmmioCreateChunk:\n jmp *g_fwd+1080(%rip)\n"
".globl mmioDescend\nmmioDescend:\n jmp *g_fwd+1088(%rip)\n"
".globl mmioFlush\nmmioFlush:\n jmp *g_fwd+1096(%rip)\n"
".globl mmioGetInfo\nmmioGetInfo:\n jmp *g_fwd+1104(%rip)\n"
".globl mmioInstallIOProcA\nmmioInstallIOProcA:\n jmp *g_fwd+1112(%rip)\n"
".globl mmioInstallIOProcW\nmmioInstallIOProcW:\n jmp *g_fwd+1120(%rip)\n"
".globl mmioOpenA\nmmioOpenA:\n jmp *g_fwd+1128(%rip)\n"
".globl mmioOpenW\nmmioOpenW:\n jmp *g_fwd+1136(%rip)\n"
".globl mmioRead\nmmioRead:\n jmp *g_fwd+1144(%rip)\n"
".globl mmioRenameA\nmmioRenameA:\n jmp *g_fwd+1152(%rip)\n"
".globl mmioRenameW\nmmioRenameW:\n jmp *g_fwd+1160(%rip)\n"
".globl mmioSeek\nmmioSeek:\n jmp *g_fwd+1168(%rip)\n"
".globl mmioSendMessage\nmmioSendMessage:\n jmp *g_fwd+1176(%rip)\n"
".globl mmioSetBuffer\nmmioSetBuffer:\n jmp *g_fwd+1184(%rip)\n"
".globl mmioSetInfo\nmmioSetInfo:\n jmp *g_fwd+1192(%rip)\n"
".globl mmioStringToFOURCCA\nmmioStringToFOURCCA:\n jmp *g_fwd+1200(%rip)\n"
".globl mmioStringToFOURCCW\nmmioStringToFOURCCW:\n jmp *g_fwd+1208(%rip)\n"
".globl mmioWrite\nmmioWrite:\n jmp *g_fwd+1216(%rip)\n"
".globl mmsystemGetVersion\nmmsystemGetVersion:\n jmp *g_fwd+1224(%rip)\n"
".globl mod32Message\nmod32Message:\n jmp *g_fwd+1232(%rip)\n"
".globl mxd32Message\nmxd32Message:\n jmp *g_fwd+1240(%rip)\n"
".globl sndPlaySoundA\nsndPlaySoundA:\n jmp *g_fwd+1248(%rip)\n"
".globl sndPlaySoundW\nsndPlaySoundW:\n jmp *g_fwd+1256(%rip)\n"
".globl tid32Message\ntid32Message:\n jmp *g_fwd+1264(%rip)\n"
".globl timeBeginPeriod\ntimeBeginPeriod:\n jmp *g_fwd+1272(%rip)\n"
".globl timeEndPeriod\ntimeEndPeriod:\n jmp *g_fwd+1280(%rip)\n"
".globl timeGetDevCaps\ntimeGetDevCaps:\n jmp *g_fwd+1288(%rip)\n"
".globl timeGetSystemTime\ntimeGetSystemTime:\n jmp *g_fwd+1296(%rip)\n"
".globl timeGetTime\ntimeGetTime:\n jmp *g_fwd+1304(%rip)\n"
".globl timeKillEvent\ntimeKillEvent:\n jmp *g_fwd+1312(%rip)\n"
".globl timeSetEvent\ntimeSetEvent:\n jmp *g_fwd+1320(%rip)\n"
".globl waveInAddBuffer\nwaveInAddBuffer:\n jmp *g_fwd+1328(%rip)\n"
".globl waveInClose\nwaveInClose:\n jmp *g_fwd+1336(%rip)\n"
".globl waveInGetDevCapsA\nwaveInGetDevCapsA:\n jmp *g_fwd+1344(%rip)\n"
".globl waveInGetDevCapsW\nwaveInGetDevCapsW:\n jmp *g_fwd+1352(%rip)\n"
".globl waveInGetErrorTextA\nwaveInGetErrorTextA:\n jmp *g_fwd+1360(%rip)\n"
".globl waveInGetErrorTextW\nwaveInGetErrorTextW:\n jmp *g_fwd+1368(%rip)\n"
".globl waveInGetID\nwaveInGetID:\n jmp *g_fwd+1376(%rip)\n"
".globl waveInGetNumDevs\nwaveInGetNumDevs:\n jmp *g_fwd+1384(%rip)\n"
".globl waveInGetPosition\nwaveInGetPosition:\n jmp *g_fwd+1392(%rip)\n"
".globl waveInMessage\nwaveInMessage:\n jmp *g_fwd+1400(%rip)\n"
".globl waveInOpen\nwaveInOpen:\n jmp *g_fwd+1408(%rip)\n"
".globl waveInPrepareHeader\nwaveInPrepareHeader:\n jmp *g_fwd+1416(%rip)\n"
".globl waveInReset\nwaveInReset:\n jmp *g_fwd+1424(%rip)\n"
".globl waveInStart\nwaveInStart:\n jmp *g_fwd+1432(%rip)\n"
".globl waveInStop\nwaveInStop:\n jmp *g_fwd+1440(%rip)\n"
".globl waveInUnprepareHeader\nwaveInUnprepareHeader:\n jmp *g_fwd+1448(%rip)\n"
".globl waveOutBreakLoop\nwaveOutBreakLoop:\n jmp *g_fwd+1456(%rip)\n"
".globl waveOutClose\nwaveOutClose:\n jmp *g_fwd+1464(%rip)\n"
".globl waveOutGetDevCapsA\nwaveOutGetDevCapsA:\n jmp *g_fwd+1472(%rip)\n"
".globl waveOutGetDevCapsW\nwaveOutGetDevCapsW:\n jmp *g_fwd+1480(%rip)\n"
".globl waveOutGetErrorTextA\nwaveOutGetErrorTextA:\n jmp *g_fwd+1488(%rip)\n"
".globl waveOutGetErrorTextW\nwaveOutGetErrorTextW:\n jmp *g_fwd+1496(%rip)\n"
".globl waveOutGetID\nwaveOutGetID:\n jmp *g_fwd+1504(%rip)\n"
".globl waveOutGetNumDevs\nwaveOutGetNumDevs:\n jmp *g_fwd+1512(%rip)\n"
".globl waveOutGetPitch\nwaveOutGetPitch:\n jmp *g_fwd+1520(%rip)\n"
".globl waveOutGetPlaybackRate\nwaveOutGetPlaybackRate:\n jmp *g_fwd+1528(%rip)\n"
".globl waveOutGetPosition\nwaveOutGetPosition:\n jmp *g_fwd+1536(%rip)\n"
".globl waveOutGetVolume\nwaveOutGetVolume:\n jmp *g_fwd+1544(%rip)\n"
".globl waveOutMessage\nwaveOutMessage:\n jmp *g_fwd+1552(%rip)\n"
".globl waveOutOpen\nwaveOutOpen:\n jmp *g_fwd+1560(%rip)\n"
".globl waveOutPause\nwaveOutPause:\n jmp *g_fwd+1568(%rip)\n"
".globl waveOutPrepareHeader\nwaveOutPrepareHeader:\n jmp *g_fwd+1576(%rip)\n"
".globl waveOutReset\nwaveOutReset:\n jmp *g_fwd+1584(%rip)\n"
".globl waveOutRestart\nwaveOutRestart:\n jmp *g_fwd+1592(%rip)\n"
".globl waveOutSetPitch\nwaveOutSetPitch:\n jmp *g_fwd+1600(%rip)\n"
".globl waveOutSetPlaybackRate\nwaveOutSetPlaybackRate:\n jmp *g_fwd+1608(%rip)\n"
".globl waveOutSetVolume\nwaveOutSetVolume:\n jmp *g_fwd+1616(%rip)\n"
".globl waveOutUnprepareHeader\nwaveOutUnprepareHeader:\n jmp *g_fwd+1624(%rip)\n"
".globl waveOutWrite\nwaveOutWrite:\n jmp *g_fwd+1632(%rip)\n"
".globl wid32Message\nwid32Message:\n jmp *g_fwd+1640(%rip)\n"
".globl winmmDbgOut\nwinmmDbgOut:\n jmp *g_fwd+1648(%rip)\n"
".globl winmmSetDebugLevel\nwinmmSetDebugLevel:\n jmp *g_fwd+1656(%rip)\n"
".globl wod32Message\nwod32Message:\n jmp *g_fwd+1664(%rip)\n"
);
