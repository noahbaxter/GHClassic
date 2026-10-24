#pragma once

#include <cstdint>

namespace gh2
{
    // Where each hooked function lives in one executable. Every hook takes
    // its address from here, so a second executable is a second table.
    struct Addresses
    {
        uint32_t entry;
        uint32_t psRndFlushPacket;
        uint32_t psRndBeginDrawing;
        uint32_t psRndEndDrawing;
        uint32_t frameLoopPolled; // where the frame loop's call of SystemPoll returns
        uint32_t psRndDrawRect;
        uint32_t psRndDoPointTests;
        uint32_t rndTestPoint; // Rnd::TestPoint
        uint32_t sphereOutsideFrustum; // operator>(const Sphere &, const Frustum &)
        uint32_t gamePanelSetGameOver; // GamePanel::SetGameOver(bool won)
        uint32_t gamePanelReset;
        uint32_t gamePanelExit;
        uint32_t winCampaignSong; // GameConfig::WinCampaignSong
        uint32_t psMeshSync;
        uint32_t psMeshUpdateFacePacket;
        uint32_t psMeshCopy;
        uint32_t psMeshDestroy;
        uint32_t psRndSyncFree; // PsRnd::SyncFree(MemHandle *)
        uint32_t thePsRnd;
        uint32_t meshStrips; // the list of strips RndMesh::Load read, a mesh's face groups in order
        uint32_t memFree;
        uint32_t memOrPoolFree;
        uint32_t psMeshDrawShowing;
        uint32_t psMultiMeshDrawShowing;
        uint32_t characterDrawShadowBuffer; // Character::DrawShadowBuffer
        uint32_t characterDrawShowing;      // Character::DrawShowing
        uint32_t characterComputeScreenSize; // Character::ComputeScreenSize(RndCam *)
        uint32_t rndSetShadowMap;           // Rnd::SetShadowMap, which PsRnd leaves as it is
        uint32_t charTrackSurfaceSetCharTexture; // CharTrackSurface::SetCharTexture(RndMat *)
        uint32_t playerMatcherOnStartStarpower;  // PlayerMatcher::OnStartStarpower
        uint32_t playerMatcherOnStopStarpower;   // PlayerMatcher::OnStopStarpower
        uint32_t psParticleSysDrawShowing;
        uint32_t updateRelativeXfm; // RndParticleSys::UpdateRelativeXfm
        uint32_t psEnvironSelect;
        uint32_t psTexSyncBitmap;
        uint32_t psTexDestroy;
        uint32_t psTexCopyFromScreen;
        uint32_t rndTextDrawShowing;
        uint32_t camShotFrameInterp;       // CamShotFrame::Interp
        uint32_t camShotFrameUpdateTarget; // CamShotFrame::UpdateTarget
        uint32_t shotTargetResolve;        // ShotTarget::Resolve
        uint32_t guitarCharacterPoll;      // GuitarCharacter::Poll
        uint32_t guitarCharacterDtor;
        uint32_t objectDirFindObject;      // ObjectDir::FindObject
        uint32_t worldXfm;       // RndTransformable::WorldXfm
        uint32_t setWorldXfm;    // RndTransformable::SetWorldXfm
        uint32_t setDirty;       // RndTransformable::SetDirty
        uint32_t psMatUpdateSphereXfm;
        uint32_t sphereXfm;      // the four quadwords PsMat::UpdateSphereXfm fills
        uint32_t playMovie;
        uint32_t muteAllTracks;  // MasterAudio::MuteAllTracks
        uint32_t muteTrack;      // MasterAudio::MuteTrack
        uint32_t unmuteTrack;    // MasterAudio::UnmuteTrack
        uint32_t masterAudioReleaseGem;
        uint32_t checkForPitchBend; // TrackWatcherImpl::CheckForPitchBend
        uint32_t sendWhammy;        // TrackWatcherImpl::SendWhammy
        uint32_t playerSetWhammyBar; // Player::SetWhammyBar
        uint32_t beatMatcherSwing;   // BeatMatcher::Swing(int slot, bool isStrum)
        uint32_t beatMatcherFretButtonDown; // BeatMatcher::FretButtonDown(int slot)
        uint32_t beatMatcherFretButtonUp;   // BeatMatcher::FretButtonUp(int slot)
        uint32_t beatMatcherNonStrumSwing;  // BeatMatcher::NonStrumSwing(int slot, bool isPress)
        uint32_t beatMatcherPoll;           // BeatMatcher::Poll(float)
        uint32_t beatMatcherDtor;           // BeatMatcher::~BeatMatcher
        uint32_t guitarWatcherJumpHook; // GuitarTrackWatcherImpl::JumpHook
        uint32_t hudPanelEnter;      // HudPanel::Enter
        uint32_t bandScoreDisplaySetScore; // BandScoreDisplay::SetScore(int)
        uint32_t bandScoreDisplaySetDigitMat; // BandScoreDisplay::SetDigitMat(int, RndMat *)
        uint32_t willBeNoStrum;      // GameGemList::WillBeNoStrum
        uint32_t trackWatcherPoll;   // TrackWatcherImpl::Poll
        uint32_t trackWatcherOnMiss; // TrackWatcherImpl::OnMiss
        uint32_t trackWatcherSendMiss;
        uint32_t guitarWatcherVtable;
        uint32_t guitarWatcherCtor;  // GuitarTrackWatcherImpl::GuitarTrackWatcherImpl
        uint32_t guitarWatcherSwing; // GuitarTrackWatcherImpl::Swing
        uint32_t guitarWatcherFretButtonUp;
        uint32_t guitarWatcherGemCanBePassed;
        uint32_t guitarWatcherSlop;
        uint32_t guitarWatcherTryToHopo;
        uint32_t guitarWatcherCheckForFretTimeout;
        uint32_t guitarWatcherCanHopo;
        uint32_t theSongDb;              // SongDB *
        uint32_t songDbGetGems;          // SongDB::GetGems(track)
        uint32_t songDbSetupStarPhrases; // SongDB::SetupStarPhrasesForTrack
        uint32_t songDbIsStarPhraseStart;
        uint32_t songDbIsInStarPhrase;
        uint32_t penalizeAsksStarPhraseStart; // where Player::Penalize's IsStarPhraseStart returns to
        uint32_t passAsksStarPhraseStart;     // and Player::Pass's
        uint32_t starPowerHitAsksStarPhraseStart; // and StarPower::Hit's
        uint32_t playerHit;              // Player::Hit
        uint32_t playerPoll;             // Player::Poll
        uint32_t performerGetMultiplier; // Performer::GetMultiplier
        uint32_t starPowerHit;           // StarPower::Hit
        uint32_t starPowerMissOrPass;    // StarPower::MissOrPass
        uint32_t crowdRatingConfigure; // CrowdRating::Configure(Difficulty), as each song's players are made
        uint32_t trackCtor;          // Track::Track
        uint32_t trackSetupFade;     // Track::SetupFade
        uint32_t trackDraw;          // Track::Draw
        uint32_t trackSurfaceGetFadeCenter; // TrackSurface::GetFadeCenter
        uint32_t trackPanelReset;         // TrackPanel::Reset
        uint32_t gemManagerJump;          // GemManager::Jump(float ms)
        uint32_t gemManagerPoll;          // GemManager::Poll
        uint32_t gemManagerHit;           // GemManager::Hit(float ms, int gem, PhraseState)
        uint32_t gemUpperBound;           // upper_bound<Gem *, Gem, less<Gem>>
        uint32_t gemPoll;                 // Gem::Poll(float...)
        uint32_t gemHit;                  // Gem::Hit
        uint32_t gemMiss;                 // Gem::Miss
        uint32_t gemRelease;              // Gem::Release
        uint32_t gemSetTailStart;         // Gem::SetTailStart(float)
        uint32_t bandSliderDrawShowing; // BandSlider::DrawShowing
        uint32_t checkBoxDrawShowing;   // CheckBox::DrawShowing
        uint32_t bandSliderDtor;        // BandSlider::~BandSlider
        uint32_t checkBoxDtor;          // CheckBox::~CheckBox
        uint32_t optionsSyncVideo; // Options::SyncVideoOptions
        uint32_t optionsSetSyncOffset;
        uint32_t beatMatchCtor;
        uint32_t playerMatcherGetSongMs;
        uint32_t ghUtlInit;        // where GH2 registers its DataFuncs
        uint32_t metaInit;
        uint32_t progressiveScanCheck;
        uint32_t splashShow;       // Splash::Show
        uint32_t seedRand;
        uint32_t dataRegisterFunc;
        uint32_t registerFactory; // Hmx::Object::RegisterFactory(Symbol, creator)
        uint32_t faderNewObject;  // Fader::NewObject
        uint32_t dataReadString;
        uint32_t dataNodeEvaluate; // DataNode::Evaluate
        uint32_t symbolCtor;       // Symbol::Symbol(const char *)
        uint32_t builtinNew;
        uint32_t builtinDelete;
        uint32_t dataNodeGetObj;   // DataNode::GetObj
        uint32_t uiGotoScreen;     // UIManager::GotoScreen
        uint32_t uiManagerPoll;
        uint32_t debugModal; // DebugModal(bool &, char *)
        uint32_t abort;
        uint32_t delayThread;
        uint32_t ctlClientPoll;
        uint32_t spuStartSend;
        uint32_t metaMusicPoll;
        uint32_t metaMusicStop;
        uint32_t metaPanelPoll;
        uint32_t metaPanelPickLoopIndex; // (int size)
        uint32_t systemConfig3; // SystemConfig(Symbol, Symbol, Symbol)
        uint32_t randomInt;     // RandomInt(lo, hi), hi exclusive
        uint32_t taskMgrAddTask; // TaskMgr::AddTask(Task *, Task::Units, float)
        uint32_t taskMgrSetUISeconds;
        uint32_t taskMgrSetSecondsBeat;
        uint32_t systemPoll;  // SystemPoll(bool): pads, files, the loader
        uint32_t synthPoll;   // Synth::Poll
        uint32_t synthEEPoll; // SynthEE::Poll
        uint32_t sequencePlay; // Sequence::Play(float, float, float)
        uint32_t streamEEPoll;
        uint32_t streamSampleFreq; // StreamEE::GetSampleFreq(int channel)
        uint32_t varTimerMs;
        uint32_t scePadRead;
        uint32_t scePadInfoAct;
        uint32_t charHairPoll;
        uint32_t rndMatLoad;
        uint32_t rndMatDtor;
        uint32_t rndMatLoadVertDyn; // where it returns to from reading a rev under 25's vertDyn
        uint32_t rndMatLoadMultiPass; // and from reading its multiPass
        uint32_t binStreamRead;     // (void *, int)
        uint32_t binStreamReadEndian; // (void *, int)
        uint32_t rndMorphCtor;
        uint32_t rndMorphDtor;
        uint32_t rndMorphSetFrame; // (frame, blend)
        uint32_t gamePanelSetExcitementLevel;
        uint32_t camShotShake;
        uint32_t freeCameraPoll; // FreeCamera::Poll
        uint32_t rndFlareDrawFlare;
        uint32_t saveData1;     // the card write, on GHMCSaveData's worker thread
        uint32_t loadData1;     // the card read, on GHMCLoadData's
        uint32_t loadData2;     // back on the main thread, where Campaign::Load runs
        uint32_t sceMcInit;
        uint32_t sceMcEnd;
        uint32_t sceMcGetInfo;
        uint32_t sceMcSync;
        uint32_t sceMcGetDir;
        uint32_t sceMcFormat;
        uint32_t sceMcFileCalls[6]; // open, mkdir, close, read, write, delete
        uint32_t localeLocalize;    // Locale::Localize(Symbol, bool)
        uint32_t bufStreamCtor; // BufStream::BufStream(void *, int, bool)
        uint32_t binStreamDtor; // BinStream::~BinStream
        uint32_t setupMcIcon;   // builds the card icon below from config/mc.dta and the disc
        uint32_t highScoreDbCtor; // HighScoreDB::HighScoreDB(const SymbolVec &songs)
        uint32_t symbolsInsertOverflow; // vector<Symbol>::_M_insert_overflow_aux
        uint32_t archiveGetFileInfo;
        uint32_t archiveIsValidBlock;
        uint32_t cdRead;
        uint32_t profileStateInitChars;
        uint32_t charsysPanelSetTypeDef;
        uint32_t charsysPanelPollCharLoading;
        uint32_t charsysPanelTrySetPriority; // (Symbol outfit, int priority)
        uint32_t charsysPanelNextCharacter;  // (int index, int direction, Symbol &outfit)
        uint32_t charsysPanelValidChar;      // (Symbol outfit)
        uint32_t playerConfigCharacterOfOutfit; // static (Symbol outfit, int &index)
        uint32_t dataVariable; // DataVariable(Symbol)
        uint32_t songProviderInitData;
        uint32_t songProviderGapSize;
        uint32_t songProviderIsActive;
        uint32_t songProviderGetSongData; // (Symbol)
        uint32_t campaignDataGetVenueForSong;
        uint32_t addLoadChar; // (list<Character *> &, const char *object, const char *character)
        uint32_t worldCrowdSetFullness; // (float flat, float whole)
        uint32_t worldCrowdBuildBillboard; // (Character *, float)
        uint32_t psRndSetDepthOfField;  // (RndCam *, float distance, float nearest, float farthest, float amount)
        uint32_t worldDirDrawShowing;
        uint32_t worldDirDtor;
        uint32_t songsInsertOverflow;     // vector<DataArray *>::_M_insert_overflow
        uint32_t headersInsertOverflow;   // vector<SongHeader>::_M_insert_overflow_aux
        uint32_t helpBarFinishLoad;
        uint32_t helpBarSetDisplay;
        uint32_t helpBarAddElement;
        uint32_t dataReadFile;     // DataReadFile(const char *)
        uint32_t systemConfig;     // SystemConfig()
        uint32_t dataArrayResize;  // DataArray::Resize(int)
        uint32_t dataArrayDtor;    // DataArray::~DataArray
        uint32_t dataArrayClone;   // DataArray::Clone(bool deep)
        uint32_t dataArraySort;    // DataArray::Sort
        uint32_t loadMgrPoll;
        uint32_t dataNodeAssign;   // DataNode::operator=(const DataNode &)
        uint32_t venueProviderInitData; // VenueProvider::InitData(RndDir *)
        uint32_t campaignCtor;
        uint32_t campaignDtor;
        uint32_t campaignIsItemUnlocked;   // Campaign::IsItemUnlocked(Symbol) const
        uint32_t campaignIsItemUnlockedOn; // Campaign::IsItemUnlocked(Symbol, Difficulty) const
        uint32_t selectGuitarProviderInitData; // SelectGuitarProvider::InitData(RndDir *)
        uint32_t charsysPanelShowChar;   // CharsysPanel::ShowChar(int, Symbol)
        uint32_t cacheModelUnload;       // CharsysPanel::CacheModel::Unload()
        uint32_t cacheModelIsLoading;    // CharsysPanel::CacheModel::IsLoading()
        uint32_t guitarElLoadGuitar;     // GuitarDisplayPanel::GuitarEl::LoadGuitar(UIProxy *, DataArray *, RndAnimatable *, float)
        uint32_t localeInit;       // Locale::Init
        uint32_t localeTerminate;  // Locale::Terminate
        uint32_t campaignStateIsEncoreSong;           // (Symbol song)
        uint32_t campaignStateIsEncoreUnlockPossible; // (Symbol song)
        uint32_t campaignStateCheckUnlockVenue;       // (Symbol song)
        uint32_t campaignStateGetNumPassedSongs;      // (Symbol venue)
        uint32_t campaignStateIsVenuePassed;          // (Symbol venue)
        uint32_t campaignStateIsUnlocked;             // (Symbol item)
        uint32_t campaignStateSetVenueUnlocked;       // (Symbol venue, bool)
        uint32_t campaignDataIsStoreSong;             // (Symbol song)
        uint32_t campaignDataGetRequiredSongs;        // (Difficulty, Symbol venue)
        uint32_t campaignDataGetNextVenue;            // (Symbol venue, Difficulty)
        uint32_t campaignItemsFind;                   // CampaignItemVec::Find(Symbol)
        uint32_t campaignItemSetPassed;               // CampaignItem::SetPassed(bool)
        uint32_t campaignGetRequiredSongs;            // Campaign's, for the venue and difficulty being played
        uint32_t campaignGetStatusProgress;           // (Difficulty)
        uint32_t campaignGetCampaign;                 // (Difficulty): that difficulty's CampaignState
        uint32_t campaignStateGetSongsBeat;
        uint32_t campaignStateGetNumSongs;
        uint32_t campaignStateGetStatusSym;
        uint32_t profileStateFinishSong;              // a career song passed, player 0's stats still up
        uint32_t songProviderGetBlurb;                // (int row)
        uint32_t songProviderGetHighScore;            // (int row)
        uint32_t makeStringInt;                       // MakeString(const char *, int)
        uint32_t playerSetMercurySwitch;              // (bool): the guitar tilted, star power deployed if ready
        // Data
        uint32_t rndCamCurrent;  // RndCam::sCurrent
        uint32_t defaultMat;     // the RndMat a mesh without one draws with
        uint32_t rndEnvironCurrent; // RndEnviron::sCurrent
        uint32_t synthServerHandler; // the handler CtlServerInit stores
        uint32_t synthServerBuffer;  // its RPC server's receive buffer
        uint32_t theTaskMgr;
        uint32_t theOptions; // Options*
        uint32_t theGameConfig; // GameConfig*
        uint32_t theCampaign;   // Campaign*
        uint32_t theLocale;
        uint32_t theLoadMgr;
        uint32_t nullStr; // char*, the empty Symbol's text
        uint32_t scoreFormat; // "%i", a song list row's score (SongProvider::GetHighScore)
        uint32_t mcBuffer;    // the save's bytes between the two halves of a load or save
        uint32_t mcOverwrite; // GHMCSaveData's overwrite argument: replace a save already there
        uint32_t mcBaseDir;   // char*, the save's directory on the card
        uint32_t mcSaveFile;  // char*, the save's file in it
        uint32_t mcIconFile;  // char*, its icon file's name
        uint32_t mcIconSize;
        uint32_t mcIconData;  // that file's bytes, read off the disc
        uint32_t mcIconSys;   // sceMcIconSys, written as icon.sys
        uint32_t arkBlockSize; // kArkBlockSize
        // RndBitmap::PixelOffset's swizzle tables, by (y / 4) & 1: 64 bytes
        // each for 8bpp, 128 for 4bpp.
        uint32_t swizzle8[2];
        uint32_t swizzle4[2];
        uint32_t streamEndMs; // the float Stream::SetJump takes for a file's end
        uint32_t scriptTaskVtable;
        uint32_t allSongsPassed;    // Campaign::AllSongsPassed(Difficulty)
        uint32_t allSongsFiveStars; // Campaign::AllSongsFiveStars(Difficulty)
    };

    // Guitar Hero II (USA), SLUS-21447.
    inline constexpr Addresses kSlus21447{
        .entry = 0x100bf0u,
        .psRndFlushPacket = 0x3d7d58u,
        .psRndBeginDrawing = 0x19af38u,
        .psRndEndDrawing = 0x19b018u,
        .frameLoopPolled = 0x100b78u,
        .psRndDrawRect = 0x19b050u,
        .psRndDoPointTests = 0x19a7f0u,
        .rndTestPoint = 0x1d55b8u,
        .sphereOutsideFrustum = 0x2d9170u,
        .gamePanelSetGameOver = 0x108178u,
        .gamePanelReset = 0x105fd8u,
        .gamePanelExit = 0x106d50u,
        .winCampaignSong = 0x126f10u,
        .psMeshSync = 0x3d4f08u,
        .psMeshUpdateFacePacket = 0x3d47f0u,
        .psMeshCopy = 0x19dc38u,
        .psMeshDestroy = 0x19dd88u,
        .psRndSyncFree = 0x19b758u,
        .thePsRnd = 0x46cb20u,
        .meshStrips = 0x471dd8u,
        .memFree = 0x2ceb48u,
        .memOrPoolFree = 0x2cf210u,
        .psMeshDrawShowing = 0x3d88d8u,
        .psMultiMeshDrawShowing = 0x1a2f00u,
        .characterDrawShadowBuffer = 0x1619c0u,
        .characterDrawShowing = 0x161c68u,
        .characterComputeScreenSize = 0x162c78u,
        .rndSetShadowMap = 0x364820u,
        .charTrackSurfaceSetCharTexture = 0x155470u,
        .playerMatcherOnStartStarpower = 0x117018u,
        .playerMatcherOnStopStarpower = 0x117060u,
        .psParticleSysDrawShowing = 0x1a2c28u,
        .updateRelativeXfm = 0x1cf7b0u,
        .psEnvironSelect = 0x1a2060u,
        .psTexSyncBitmap = 0x1a13a8u,
        .psTexDestroy = 0x1a0f18u,
        .psTexCopyFromScreen = 0x1a0d68u,
        .rndTextDrawShowing = 0x1dc380u,
        .camShotFrameInterp = 0x2665a0u,
        .camShotFrameUpdateTarget = 0x266df8u,
        .shotTargetResolve = 0x261c10u,
        .guitarCharacterPoll = 0x10ac90u,
        .guitarCharacterDtor = 0x109f58u,
        .objectDirFindObject = 0x2bb4f0u,
        .worldXfm = 0x3d8ea0u,
        .setWorldXfm = 0x1dd7b8u,
        .setDirty = 0x1dd748u,
        .psMatUpdateSphereXfm = 0x19cb78u,
        .sphereXfm = 0x46d490u,
        .playMovie = 0x21bb60u,
        .muteAllTracks = 0x23a3e8u,
        .muteTrack = 0x23a2c0u,
        .unmuteTrack = 0x23a370u,
        .masterAudioReleaseGem = 0x239ec8u,
        .checkForPitchBend = 0x248e38u,
        .sendWhammy = 0x249b98u,
        .playerSetWhammyBar = 0x111d88u,
        .beatMatcherSwing = 0x2356b8u,
        .beatMatcherFretButtonDown = 0x235568u,
        .beatMatcherFretButtonUp = 0x235610u,
        .beatMatcherNonStrumSwing = 0x2358f0u,
        .beatMatcherPoll = 0x235340u,
        .beatMatcherDtor = 0x234b08u,
        .guitarWatcherJumpHook = 0x24d4e0u,
        .hudPanelEnter = 0x12ba00u,
        .bandScoreDisplaySetScore = 0x291af8u,
        .bandScoreDisplaySetDigitMat = 0x291c48u,
        .willBeNoStrum = 0x24d360u,
        .trackWatcherPoll = 0x2484e0u,
        .trackWatcherOnMiss = 0x249338u,
        .trackWatcherSendMiss = 0x249598u,
        .guitarWatcherVtable = 0x3efb10u,
        .guitarWatcherCtor = 0x24d400u,
        .guitarWatcherSwing = 0x24d518u,
        .guitarWatcherFretButtonUp = 0x24d938u,
        .guitarWatcherGemCanBePassed = 0x24da40u,
        .guitarWatcherSlop = 0x24da50u,
        .guitarWatcherTryToHopo = 0x24dc20u,
        .guitarWatcherCheckForFretTimeout = 0x24de48u,
        .guitarWatcherCanHopo = 0x24df30u,
        .theSongDb = 0x3da2f8u,
        .songDbGetGems = 0x11cf58u,
        .songDbSetupStarPhrases = 0x11d850u,
        .songDbIsStarPhraseStart = 0x11da68u,
        .songDbIsInStarPhrase = 0x11da90u,
        .penalizeAsksStarPhraseStart = 0x1117d0u,
        .passAsksStarPhraseStart = 0x111410u,
        .starPowerHitAsksStarPhraseStart = 0x11e468u,
        .playerHit = 0x1111d0u,
        .playerPoll = 0x1110d8u,
        .performerGetMultiplier = 0x10f1a0u,
        .starPowerHit = 0x11e2b8u,
        .starPowerMissOrPass = 0x11e6a8u,
        .crowdRatingConfigure = 0x124988u,
        .trackCtor = 0x14fa70u,
        .trackSetupFade = 0x150908u,
        .trackDraw = 0x151188u,
        .trackSurfaceGetFadeCenter = 0x154fb0u,
        .trackPanelReset = 0x153c38u,
        .gemManagerJump = 0x156da0u,
        .gemManagerPoll = 0x1564f8u,
        .gemManagerHit = 0x156b08u,
        .gemUpperBound = 0x32b490u,
        .gemPoll = 0x157868u,
        .gemHit = 0x157c20u,
        .gemMiss = 0x157bc0u,
        .gemRelease = 0x157c80u,
        .gemSetTailStart = 0x157de0u,
        .bandSliderDrawShowing = 0x296bd8u,
        .checkBoxDrawShowing = 0x29bde8u,
        .bandSliderDtor = 0x296528u,
        .checkBoxDtor = 0x29b9b8u,
        .optionsSyncVideo = 0x10db80u,
        .optionsSetSyncOffset = 0x10ded8u,
        .beatMatchCtor = 0x120f48u,
        .playerMatcherGetSongMs = 0x115738u,
        .ghUtlInit = 0x11fbf0u,
        .metaInit = 0x133f78u,
        .progressiveScanCheck = 0x100058u,
        .splashShow = 0x2139f0u,
        .seedRand = 0x2d9cc8u,
        .dataRegisterFunc = 0x2b2c80u,
        .registerFactory = 0x2c0ed0u,
        .faderNewObject = 0x37f160u,
        .dataReadString = 0x2b28d0u,
        .dataNodeEvaluate = 0x2b7d38u,
        .symbolCtor = 0x2d3a48u,
        .builtinNew = 0x2cf138u,
        .builtinDelete = 0x2cf160u,
        .dataNodeGetObj = 0x2b7f80u,
        .uiGotoScreen = 0x214d48u,
        .uiManagerPoll = 0x214510u,
        .debugModal = 0x105a88u,
        .abort = 0x307b80u,
        .delayThread = 0x2f7dd8u,
        .ctlClientPoll = 0x22dc68u,
        .spuStartSend = 0x231a78u,
        .metaMusicPoll = 0x21f180u,
        .metaMusicStop = 0x21f668u,
        .metaPanelPoll = 0x134850u,
        .metaPanelPickLoopIndex = 0x1349d8u,
        .systemConfig3 = 0x2a8700u,
        .randomInt = 0x2d9d10u,
        .taskMgrAddTask = 0x2c6b18u,
        .taskMgrSetUISeconds = 0x2c6738u,
        .taskMgrSetSecondsBeat = 0x2c6798u,
        .systemPoll = 0x2a85d0u,
        .synthPoll = 0x225cc8u,
        .synthEEPoll = 0x22cc80u,
        .sequencePlay = 0x21fa68u,
        .streamEEPoll = 0x2308a0u,
        .streamSampleFreq = 0x2314e8u,
        .varTimerMs = 0x2d4018u,
        .scePadRead = 0x2f2c48u,
        .scePadInfoAct = 0x2f2e58u,
        .charHairPoll = 0x176fb8u,
        .rndMatLoad = 0x1bfc00u,
        .rndMatDtor = 0x35bb08u,
        .rndMatLoadVertDyn = 0x1c0038u,
        .rndMatLoadMultiPass = 0x1c0270u,
        .binStreamRead = 0x2c8c50u,
        .binStreamReadEndian = 0x2c8e20u,
        .rndMorphCtor = 0x200b98u,
        .rndMorphDtor = 0x374f90u,
        .rndMorphSetFrame = 0x201048u,
        .gamePanelSetExcitementLevel = 0x108978u,
        .camShotShake = 0x262f38u,
        .freeCameraPoll = 0x1b9370u,
        .rndFlareDrawFlare = 0x1f9330u,
        .saveData1 = 0x14b3e0u,
        .loadData1 = 0x14ba00u,
        .loadData2 = 0x14bae0u,
        .sceMcInit = 0x2f3860u,
        .sceMcEnd = 0x2f3af8u,
        .sceMcGetInfo = 0x2f4240u,
        .sceMcSync = 0x2f4120u,
        .sceMcGetDir = 0x2f43c0u,
        .sceMcFormat = 0x2f4518u,
        .sceMcFileCalls = {0x2f3bc0u, 0x2f3ce8u, 0x2f3d20u, 0x2f3e90u, 0x2f3fa8u, 0x2f45e8u},
        .localeLocalize = 0x2cbaf8u,
        .bufStreamCtor = 0x2c9268u,
        .binStreamDtor = 0x2c8b78u,
        .setupMcIcon = 0x14b098u,
        .highScoreDbCtor = 0x13cef0u,
        .symbolsInsertOverflow = 0x313a40u,
        .archiveGetFileInfo = 0x2ac618u,
        .archiveIsValidBlock = 0x2ac9e8u,
        .cdRead = 0x2ae9c0u,
        .profileStateInitChars = 0x13ad80u,
        .charsysPanelSetTypeDef = 0x141bf8u,
        .charsysPanelPollCharLoading = 0x142ea8u,
        .charsysPanelTrySetPriority = 0x142790u,
        .charsysPanelNextCharacter = 0x142c70u,
        .charsysPanelValidChar = 0x142bc0u,
        .playerConfigCharacterOfOutfit = 0x114030u,
        .dataVariable = 0x2b7b00u,
        .songProviderInitData = 0x117448u,
        .songProviderGapSize = 0x1181d8u,
        .songProviderIsActive = 0x117a50u,
        .songProviderGetSongData = 0x118318u,
        .campaignDataGetVenueForSong = 0x1312a0u,
        .addLoadChar = 0x128778u,
        .worldCrowdSetFullness = 0x26bdf0u,
        .worldCrowdBuildBillboard = 0x26bba0u,
        .psRndSetDepthOfField = 0x19a798u,
        .worldDirDrawShowing = 0x26f6b8u,
        .worldDirDtor = 0x26ddb8u,
        .songsInsertOverflow = 0x3177e0u,
        .headersInsertOverflow = 0x3175d8u,
        .helpBarFinishLoad = 0x149f40u,
        .helpBarSetDisplay = 0x14a2f0u,
        .helpBarAddElement = 0x14a1e0u,
        .dataReadFile = 0x2b2928u,
        .systemConfig = 0x2a8680u,
        .dataArrayResize = 0x2afbd8u,
        .dataArrayDtor = 0x2b07d0u,
        .dataArrayClone = 0x2b0558u,
        .dataArraySort = 0x2b08d8u,
        .loadMgrPoll = 0x2cc218u,
        .dataNodeAssign = 0x2b8298u,
        .venueProviderInitData = 0x118a90u,
        .campaignCtor = 0x12cf00u,
        .campaignDtor = 0x12d0c0u,
        .campaignIsItemUnlocked = 0x12d490u,
        .campaignIsItemUnlockedOn = 0x12d5a0u,
        .selectGuitarProviderInitData = 0x119060u,
        .charsysPanelShowChar = 0x143120u,
        .cacheModelUnload = 0x141250u,
        .cacheModelIsLoading = 0x141730u,
        .guitarElLoadGuitar = 0x1470d8u,
        .localeInit = 0x2cb798u,
        .localeTerminate = 0x2cba98u,
        .campaignStateIsEncoreSong = 0x131d30u,
        .campaignStateIsEncoreUnlockPossible = 0x132b18u,
        .campaignStateCheckUnlockVenue = 0x1322c8u,
        .campaignStateGetNumPassedSongs = 0x132450u,
        .campaignStateIsVenuePassed = 0x132428u,
        .campaignStateIsUnlocked = 0x131c70u,
        .campaignStateSetVenueUnlocked = 0x132500u,
        .campaignDataIsStoreSong = 0x131388u,
        .campaignDataGetRequiredSongs = 0x1313d8u,
        .campaignDataGetNextVenue = 0x1310c8u,
        .campaignItemsFind = 0x140cf8u,
        .campaignItemSetPassed = 0x140a90u,
        .campaignGetRequiredSongs = 0x12e2d8u,
        .campaignGetStatusProgress = 0x12f828u,
        .campaignGetCampaign = 0x12e5e0u,
        .campaignStateGetSongsBeat = 0x132f00u,
        .campaignStateGetNumSongs = 0x133020u,
        .campaignStateGetStatusSym = 0x132760u,
        .profileStateFinishSong = 0x13ba20u,
        .songProviderGetBlurb = 0x117df8u,
        .songProviderGetHighScore = 0x117b38u,
        .makeStringInt = 0x3147a8u,
        .playerSetMercurySwitch = 0x111940u,
        .rndCamCurrent = 0x3de348u,
        .defaultMat = 0x3da4f0u,
        .rndEnvironCurrent = 0x3de358u,
        .synthServerHandler = 0x3de454u,
        .synthServerBuffer = 0x484340u,
        .theTaskMgr = 0x51ee40u,
        .theOptions = 0x3da2e8u,
        .theGameConfig = 0x3da308u,
        .theCampaign = 0x3da31cu,
        .theLocale = 0x51f1b8u,
        .theLoadMgr = 0x51f1d8u,
        .nullStr = 0x3de688u,
        .scoreFormat = 0x3f8408u,
        .mcBuffer = 0x3da368u,
        .mcOverwrite = 0x3da394u,
        .mcBaseDir = 0x3da374u,
        .mcSaveFile = 0x3da370u,
        .mcIconFile = 0x3da378u,
        .mcIconSize = 0x3da37cu,
        .mcIconData = 0x3da380u,
        .mcIconSys = 0x46a908u,
        .arkBlockSize = 0x45f940u,
        .swizzle8 = {0x3de1c8u, 0x3de208u},
        .swizzle4 = {0x3de248u, 0x3de2c8u},
        .streamEndMs = 0x43311cu,
        .scriptTaskVtable = 0x3f3510u, // as ScriptTask's ctor (0x2c5040) stores it
        .allSongsPassed = 0x12d890u,
        .allSongsFiveStars = 0x12d8d8u,
    };
}
