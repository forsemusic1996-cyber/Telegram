-- REAPER Telegram uploader.
-- Put this file in the REAPER Scripts directory.
-- Put Telegram.ini next to this file and fill in BOT_TOKEN and CHAT_ID.

local SCRIPT_DIR = debug.getinfo(1, "S").source:match("^@(.+)[/\\][^/\\]+$")
local CONFIG_PATH = SCRIPT_DIR .. "\\Telegram.ini"

local function readConfig(path)
    local result = {}
    local f = io.open(path, "r")
    if not f then return result end
    for line in f:lines() do
        local key, value = line:match("^%s*([%w_]+)%s*=%s*(.-)%s*$")
        if key and value and value ~= "" and not value:match("^#") then
            result[key] = value
        end
    end
    f:close()
    return result
end

local cfg = readConfig(CONFIG_PATH)
local BOT_TOKEN = cfg.BOT_TOKEN or ""
local CHAT_ID = cfg.CHAT_ID or ""

local function selectedItemFile()
    local item = reaper.GetSelectedMediaItem(0, 0)
    if not item then return nil end
    local take = reaper.GetActiveTake(item)
    if not take or reaper.TakeIsMIDI(take) then return nil end
    local source = reaper.GetMediaItemTake_Source(take)
    local path = reaper.GetMediaSourceFileName(source, "")
    return path ~= "" and path or nil
end

local function fileName(path)
    return path:match("[^/\\]+$") or path
end

local ctx = reaper.ImGui_CreateContext("Telegram")
local font = reaper.ImGui_CreateFont("Arial", 16)
reaper.ImGui_Attach(ctx, font)

local job = nil
local status = "Ready"
local statusTime = 0

local function startUpload(path)
    if BOT_TOKEN == "" or CHAT_ID == "" then
        status = "Fill Telegram.ini"
        return
    end
    if not reaper.APIExists("Telegram_StartUpload") then
        status = "Telegram.dll is not installed"
        return
    end
    if job then
        status = "Upload already running"
        return
    end

    local caption = fileName(path)
    job = reaper.Telegram_StartUpload(BOT_TOKEN, CHAT_ID, path, caption)
    if not job or job < 0 then
        job = nil
        status = "Could not start upload"
    else
        status = "Uploading..."
    end
    statusTime = reaper.time_precise()
end

local function pollUpload()
    if not job then return end
    local state = reaper.Telegram_PollUpload(job)
    if state == 1 then
        status = "File sent"
        reaper.Telegram_ReleaseUpload(job)
        job = nil
        statusTime = reaper.time_precise()
    elseif state == -1 then
        status = "Error: " .. tostring(reaper.Telegram_UploadError(job))
        reaper.Telegram_ReleaseUpload(job)
        job = nil
        statusTime = reaper.time_precise()
    end
end

local function loop()
    pollUpload()
    -- Recent ReaImGui versions require the base font size as the third argument.
    reaper.ImGui_PushFont(ctx, font, 16.0)
    reaper.ImGui_SetNextWindowSize(ctx, 310, 120, reaper.ImGui_Cond_FirstUseEver())
    local visible, open = reaper.ImGui_Begin(ctx, "Telegram", true)
    if visible then
        if reaper.ImGui_Button(ctx, "Send selected item") then
            local path = selectedItemFile()
            if path then
                startUpload(path)
            else
                status = "Select an audio item"
            end
        end
        reaper.ImGui_Text(ctx, status)
        reaper.ImGui_Text(ctx, "Config: " .. CONFIG_PATH)
        reaper.ImGui_End(ctx)
    end
    reaper.ImGui_PopFont(ctx)
    if open then reaper.defer(loop) end
end

loop()
