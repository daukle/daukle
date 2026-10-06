--- Registers one task per entry of a manifest table, reading the manifest by
--- the name daukle was started from rather than by a spelling of its own.
daukle.plugin{ api = 1, uses = { "read", "parse" } }

daukle.toolchain{ name = "probe", generate = function() return {} end }

local config = daukle.parse(daukle.read(daukle.manifest), daukle.manifest)
local probe = config.toolchains ~= nil and config.toolchains.probe or nil
local scripts = probe ~= nil and probe.scripts or nil

if scripts ~= nil then
  local names = {}
  for name in pairs(scripts) do names[#names + 1] = name end
  table.sort(names)
  for index = 1, #names do
    daukle.task{ name = "probe:" .. names[index], run = function() end }
  end
end
