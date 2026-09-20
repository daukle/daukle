daukle.plugin{ api = 1, uses = { "json_set", "parse" } }

local LEDGER_PATH = "daukle.managed"

local function read_ledger(text, configuration)
  local document = daukle.parse(text, "package.json")
  local managed = document.daukle and document.daukle.managed
  local owned = managed and managed[configuration]
  if type(owned) ~= "table" then return {} end
  return owned
end

local function contains(entries, package)
  for index = 1, #entries do
    if entries[index].package == package then return true end
  end
  return false
end

daukle.language{
  name = "npm",
  apply = function(consumer, resolved, text)
    local configuration = consumer.configuration
    if configuration == nil or configuration == "" then
      error("consumer has no \"configuration\" for the npm language plugin")
    end
    if configuration:find("%.") then
      error("npm \"configuration\" \"" .. configuration .. "\" must name one member, not a path")
    end

    local entries = {}
    for index = 1, #resolved do
      local entry = resolved[index]
      if type(entry.block.package) ~= "string" or type(entry.block.range) ~= "string" then
        error("modules." .. entry.module .. ".npm needs \"package\" and \"range\"")
      end
      entries[index] = { package = entry.block.package, range = entry.block.range }
    end

    local ledger = read_ledger(text, configuration)
    -- a package.json daukle owns nothing in must come back byte identical
    if #entries == 0 and #ledger == 0 then return text end

    for index = 1, #entries do
      text = daukle.json_set(text, configuration, entries[index].package, entries[index].range)
    end
    for index = 1, #ledger do
      if not contains(entries, ledger[index]) then
        text = daukle.json_set(text, configuration, ledger[index], nil)
      end
    end

    local packages = {}
    for index = 1, #entries do packages[index] = entries[index].package end
    table.sort(packages)
    return daukle.json_set(text, LEDGER_PATH, configuration, packages)
  end,
}
