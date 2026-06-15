-- HARP REAPER e2e ReaScript (run as __startup.lua by scripts/reaper-e2e.sh).
--
-- Builds a project (HARP RefDev on a track + a held chord) and renders it to a
-- WAV through the device. The render is deferred so it runs in REAPER's main
-- loop (it blocks/no-ops if invoked during __startup init). The harness sets
-- the output dir/name/status path via environment variables and pins the
-- device's parameter state before each run so two renders are byte-identical.
local OUTDIR = os.getenv("HARP_E2E_OUTDIR") or "/tmp/harp-reaper"
local NAME = os.getenv("HARP_E2E_NAME") or "render"
local STATUS = os.getenv("HARP_E2E_STATUS") or (OUTDIR .. "/status.txt")
local function w(s) local f = io.open(STATUS, "w"); if f then f:write(s .. "\n"); f:close() end end

local ok, err = pcall(function()
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
  reaper.GetSetProjectInfo(0, "RENDER_BOUNDSFLAG", 0, true) -- custom time range
  reaper.GetSetProjectInfo(0, "RENDER_STARTPOS", 0.0, true)
  reaper.GetSetProjectInfo(0, "RENDER_ENDPOS", 2.0, true)
  reaper.GetSetProjectInfo(0, "RENDER_TAILFLAG", 0, true)   -- no tail (drone never silent)
  reaper.GetSetProjectInfo(0, "RENDER_SRATE", 48000, true)
  reaper.GetSetProjectInfo(0, "RENDER_CHANNELS", 2, true)
  reaper.GetSetProjectInfo_String(0, "RENDER_FORMAT", "ZXZhdxgAAAA=", true) -- 24-bit WAV
  reaper.GetSetProjectInfo_String(0, "RENDER_FILE", OUTDIR, true)   -- output DIRECTORY
  reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", NAME, true)  -- basename (no ext)
  w("built")
end)
if not ok then w("ERROR(build): " .. tostring(err)); return end

-- defer the render into the main loop, a few cycles after init settles
local cyc = 0
local function tick()
  cyc = cyc + 1
  if cyc < 5 then reaper.defer(tick); return end
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
