daukle.plugin{ api = 1, uses = { "fetch", "cache", "env", "json_parse" } }

--- DAUKLE_TOKEN wins over GITHUB_TOKEN, so a daukle-specific token cannot be
--- shadowed by whatever the surrounding CI already exports. The same precedence
--- as plugins/github.lua, which is a pair that has to be kept in step by hand
--- until a plugin may depend on a plugin.
local function authorization()
  local token = daukle.env("DAUKLE_TOKEN")
  if token == nil or token == "" then token = daukle.env("GITHUB_TOKEN") end
  if token == nil or token == "" then return nil end
  return { Authorization = "Bearer " .. token }
end

local function parse_version(text)
  local major, minor, patch = text:match("^(%d+)%.(%d+)%.(%d+)$")
  if major == nil then return nil end
  return { major = tonumber(major), minor = tonumber(minor), patch = tonumber(patch) }
end

--- The three range kinds src/semver.c implements, and no others: an exact
--- version, "^" up to the next major, "~" up to the next minor. Core no longer
--- offers range matching to anything, so this is the honest cost of moving
--- coordinates out of C rather than an omission.
local function parse_range(text)
  local first = text:sub(1, 1)
  if first == "^" or first == "~" then
    local base = parse_version(text:sub(2))
    if base == nil then return nil end
    return { kind = first, base = base }
  end
  local base = parse_version(text)
  if base == nil then return nil end
  return { kind = "=", base = base }
end

local function at_least(version, base)
  if version.major ~= base.major then return version.major > base.major end
  if version.minor ~= base.minor then return version.minor > base.minor end
  return version.patch >= base.patch
end

local function satisfies(range, version)
  local base = range.base
  if range.kind == "=" then
    return version.major == base.major and version.minor == base.minor
       and version.patch == base.patch
  end
  if not at_least(version, base) then return false end
  if range.kind == "^" then return version.major == base.major end
  return version.major == base.major and version.minor == base.minor
end

local function greater(a, b)
  if a.major ~= b.major then return a.major > b.major end
  if a.minor ~= b.minor then return a.minor > b.minor end
  return a.patch > b.patch
end

--- An absent key takes the fallback, but a key written with the wrong type is
--- the manifest author's mistake and must name where, since a non-string would
--- otherwise surface only as a concatenation failure inside this plugin.
local function optional_string(block, key, repo, fallback)
  local value = block[key]
  if value == nil then return fallback end
  if type(value) ~= "string" then
    error("the resolver block for \"" .. repo .. "\" has a non-string \"" .. key .. "\"")
  end
  return value
end

local function asset_url(release, name)
  local assets = release.assets
  if type(assets) ~= "table" then return nil end
  for _, asset in ipairs(assets) do
    if asset.name == name then return asset.browser_download_url end
  end
  return nil
end

daukle.resolver{
  resolve = function(coordinate, block)
    local repo, range_text = coordinate:match("^([^@]+)@(.+)$")
    if repo == nil then
      error("\"" .. coordinate .. "\" needs a version, as \"owner/name@range\"")
    end
    if repo:match("^[^/]+/[^/]+$") == nil then
      error("\"" .. repo .. "\" must be \"owner/name\"")
    end

    local range = parse_range(range_text)
    if range == nil then
      error("\"" .. range_text .. "\" is not a version range; write an exact version like"
            .. " \"1.2.3\", a caret range like \"^1.2.3\", or a tilde range like \"~1.2.3\"")
    end

    local api = optional_string(block, "api", repo, "https://api.github.com")
    local asset = optional_string(block, "asset", repo, "plugin.lua")
    local index = api .. "/repos/" .. repo .. "/releases"

    --- The one interior slash daukle.cache's project slot permits, exactly the "owner/name" shape.
    local cache_project = repo

    local body = daukle.cache(cache_project, range_text, index, function()
      return daukle.fetch(index, authorization())
    end)

    local releases = daukle.json_parse(body)
    if type(releases) ~= "table" then error("the releases response is not a json array") end

    local best, best_release = nil, nil
    -- A tag that is not a version (a doc tag, a marker) is skipped rather than
    -- refused: not every tag in a repository is a release of this plugin.
    for _, release in ipairs(releases) do
      local version = type(release.tag_name) == "string" and parse_version(release.tag_name) or nil
      if version ~= nil and satisfies(range, version) then
        if best == nil or greater(version, best) then
          best, best_release = version, release
        end
      end
    end

    if best == nil then
      error("no release of \"" .. repo .. "\" satisfies \"" .. range_text .. "\"")
    end

    local url = asset_url(best_release, asset)
    if url == nil then
      error("the release " .. best.major .. "." .. best.minor .. "." .. best.patch
            .. " of \"" .. repo .. "\" has no " .. asset .. " asset")
    end

    --- The asset carries the same headers the index did: a private repository
    --- serves neither without them, and core forwards whatever a resolver
    --- returns here without reading it.
    return {
      url = url,
      resolved = best.major .. "." .. best.minor .. "." .. best.patch,
      headers = authorization(),
    }
  end,
}
