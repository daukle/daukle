daukle.plugin{ api = 1, uses = { "fetch", "cache", "env", "parse" } }

--- The replacement is a function so a version containing "%" is not read as a
--- capture reference, and the extra parentheses drop gsub's match count.
local function substitute_version(template, version)
  return (template:gsub("{version}", function() return version end))
end

--- An absent key takes the fallback, but a key written with the wrong type is
--- the manifest author's mistake and must name where, since a non-string would
--- otherwise surface only as a concatenation failure inside this plugin.
local function optional_string(block, key, project, fallback)
  local value = block[key]
  if value == nil then return fallback end
  if type(value) ~= "string" then
    error("sources." .. project .. "." .. key .. " must be a string")
  end
  return value
end

--- DAUKLE_TOKEN wins over GITHUB_TOKEN, so a daukle-specific token cannot be
--- shadowed by whatever the surrounding CI already exports.
local function authorization()
  local token = daukle.env("DAUKLE_TOKEN")
  if token == nil or token == "" then token = daukle.env("GITHUB_TOKEN") end
  if token == nil or token == "" then return nil end
  return { Authorization = "Bearer " .. token }
end

daukle.source{
  name = "github-releases",
  load = function(project, block, base_dir)
    if type(block.repo) ~= "string" then error("sources." .. project .. " has no \"repo\"") end
    if type(block.version) ~= "string" then error("sources." .. project .. " has no \"version\"") end

    local asset = optional_string(block, "asset", project, "daukle.toml")
    local tag_template = optional_string(block, "tag", project, "{version}")
    local tag = substitute_version(tag_template, block.version)
    local url = "https://github.com/" .. block.repo .. "/releases/download/" .. tag .. "/" .. asset

    -- daukle.cache writes whatever the producer returns, so the parse has to
    -- happen here too or an unparseable body would be served from cache next run.
    local text = daukle.cache(project, block.version, url, function()
      local body = daukle.fetch(url, authorization())
      daukle.parse(body, asset)
      return body
    end)
    return daukle.parse(text, asset)
  end,
}
