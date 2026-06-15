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

local function set_render_cfg()
  reaper.GetSetProjectInfo(0, "RENDER_BOUNDSFLAG", 0, true) -- custom time range
  reaper.GetSetProjectInfo(0, "RENDER_STARTPOS", 0.0, true)
  reaper.GetSetProjectInfo(0, "RENDER_ENDPOS", 2.0, true)
  reaper.GetSetProjectInfo(0, "RENDER_TAILFLAG", 0, true)   -- no tail (drone never silent)
  reaper.GetSetProjectInfo(0, "RENDER_SRATE", 48000, true)
  reaper.GetSetProjectInfo(0, "RENDER_CHANNELS", 2, true)
  reaper.GetSetProjectInfo_String(0, "RENDER_FORMAT", "ZXZhdxgAAAA=", true) -- 24-bit WAV
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
    if SAVE then reaper.Main_SaveProjectEx(0, SAVE, 0) end -- Recall Bundle -> .rpp
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
    local p = OUTDIR .. "/" .. NAME .. ".wav"
    local f = io.open(p, "rb")
    local n = f and f:seek("end") or -1
    if f then f:close() end
    w("rendered bytes=" .. tostring(n))
  end)
  if not ok2 then w("ERROR(render): " .. tostring(err2)) end
end
reaper.defer(tick)
