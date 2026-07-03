-- HARP REAPER e2e ReaScript (run as __startup.lua by scripts/reaper-e2e.sh).
--
-- Two modes, selected by HARP_E2E_MODE:
--   build  — new project: a track with HARP RefDev + a held chord; optionally
--            saved to HARP_E2E_SAVE (so its Recall Bundle lands in the .rpp).
--   open   — open the project at HARP_E2E_PROJECT. Loading it restores the
--            plugin's saved state, which re-pushes the Recall Bundle to the
--            device (the recall-through-a-real-DAW path under test).
-- Either way it renders to OUTDIR/NAME.wav. The render is deferred so it runs
-- in REAPER's main loop (it no-ops during __startup init), with enough settle
-- cycles for an open-mode recall push to reach the device before the render.
local MODE = os.getenv("HARP_E2E_MODE") or "build"
local OUTDIR = os.getenv("HARP_E2E_OUTDIR") or "/tmp/harp-reaper"
local NAME = os.getenv("HARP_E2E_NAME") or "render"
local SAVE = os.getenv("HARP_E2E_SAVE")        -- build: save the project here
local PROJECT = os.getenv("HARP_E2E_PROJECT")  -- open: project to load
local STATUS = os.getenv("HARP_E2E_STATUS") or (OUTDIR .. "/status.txt")
local function w(s) local f = io.open(STATUS, "w"); if f then f:write(s .. "\n"); f:close() end end
w("loaded") -- earliest marker: proves __startup ran (vs an empty status = it didn't)

local function set_render_cfg()
  reaper.GetSetProjectInfo(0, "RENDER_BOUNDSFLAG", 0, true) -- custom time range
  reaper.GetSetProjectInfo(0, "RENDER_STARTPOS", 0.0, true)
  reaper.GetSetProjectInfo(0, "RENDER_ENDPOS", 2.0, true)
  reaper.GetSetProjectInfo(0, "RENDER_TAILFLAG", 0, true)   -- no tail (notes are within the render window)
  reaper.GetSetProjectInfo(0, "RENDER_SRATE", 48000, true)
  reaper.GetSetProjectInfo(0, "RENDER_CHANNELS", 2, true)
  -- Determinism hygiene (harmless to the Linux hw self-compare — both its renders share
  -- this cfg — and required for a meaningful byte-pin): no dither/noise-shaping, no
  -- normalize/brickwall, and force the project SR to equal the render SR so no SRC runs.
  reaper.GetSetProjectInfo(0, "RENDER_DITHER", 0, true)      -- no dither / noise shaping
  reaper.GetSetProjectInfo(0, "RENDER_NORMALIZE", 0, true)   -- no normalize / brickwall limit
  reaper.GetSetProjectInfo(0, "PROJECT_SRATE_USE", 1, true)  -- pin the project SR ...
  reaper.GetSetProjectInfo(0, "PROJECT_SRATE", 48000, true)  -- ... to 48k (== RENDER_SRATE, no SRC)
  -- WAV bit depth is the 5th byte of REAPER's 'evaw' render blob: 0x18=24-bit PCM,
  -- 0x20=32-bit (WAV offers float-only at 32/64, so 0x20 == 32-bit FLOAT). The env
  -- selector keeps this ONE cross-platform script serving both suites: hw.yml (Linux,
  -- USB) keeps its proven 24-bit render UNCHANGED (env unset), while the Windows
  -- reaper-e2e.yml loopback sets HARP_E2E_RENDER_FMT=f32 for a lossless float capture
  -- of the engine output (so a Tier-B byte-pin reflects the true samples).
  local blob = (os.getenv("HARP_E2E_RENDER_FMT") == "f32")
      and "ZXZhdyAAAAA="   -- evaw + 32  (32-bit float WAV)
      or  "ZXZhdxgAAAA="   -- evaw + 24  (24-bit PCM WAV, the historical default)
  reaper.GetSetProjectInfo_String(0, "RENDER_FORMAT", blob, true)
  reaper.GetSetProjectInfo_String(0, "RENDER_FILE", OUTDIR, true)   -- output DIRECTORY
  reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", NAME, true)  -- basename (no ext)
end

local ok, err = pcall(function()
  if MODE == "open" then
    if not PROJECT then error("open mode needs HARP_E2E_PROJECT") end
    reaper.Main_openProject("noprompt:" .. PROJECT) -- restores plugin state -> recall push
    set_render_cfg() -- redirect this run's output (the .rpp carries its own paths)
    w("opened")
  else
    reaper.Main_OnCommand(40859, 0) -- new project tab
    reaper.InsertTrackAtIndex(0, false)
    local tr = reaper.GetTrack(0, 0)
    local fx = reaper.TrackFX_AddByName(tr, "HARP RefDev", false, -1)
    if fx < 0 then error("HARP RefDev not found (plugin not scanned?)") end
    local item = reaper.CreateNewMIDIItemInProj(tr, 0, 2.0, false)
    local take = reaper.GetActiveTake(item)
    for _, p in ipairs({48, 55, 62}) do
      reaper.MIDI_InsertNote(take, false, false, 0, 1920, 0, p, 100, false)
    end
    reaper.MIDI_Sort(take)
    set_render_cfg()
    -- NB: the project is saved AFTER the first render (see tick), not here —
    -- the plugin has not connected yet, so getStateBundle would capture nothing.
    w("built")
  end
end)
if not ok then w("ERROR(setup): " .. tostring(err)); return end

-- defer the render into the main loop, after init (and any recall push) settles
local cyc = 0
local function tick()
  cyc = cyc + 1
  if cyc < 8 then reaper.defer(tick); return end
  local ok2, err2 = pcall(function()
    reaper.Main_OnCommand(42230, 0) -- render, most recent settings, no dialog
    -- Save AFTER the render: rendering forces the plugin to connect, so now
    -- getStateBundle captures the live device state into the .rpp. (Saving in
    -- __startup ran before connect -> empty bundle -> nothing to recall.)
    if MODE ~= "open" and SAVE then reaper.Main_SaveProjectEx(0, SAVE, 0) end
    local p = OUTDIR .. "/" .. NAME .. ".wav"
    local f = io.open(p, "rb")
    local n = f and f:seek("end") or -1
    if f then f:close() end
    w("rendered bytes=" .. tostring(n))
  end)
  if not ok2 then w("ERROR(render): " .. tostring(err2)) end
end
reaper.defer(tick)
