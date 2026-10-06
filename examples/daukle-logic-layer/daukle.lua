-- The LOGIC half, and daukle's answer to a Gradle build script.
--
-- Everything here is something a static manifest cannot say. It is optional:
-- a project with no daukle.lua never starts an interpreter, and most projects
-- do not need one.

-- One list, named once. In TOML the same package appears in the consumer's
-- dependency table and again in its module list, and the two drift. Here
-- adding a package is one line and both follow.
local WANTED = {
  { project = "example/greeter", version = "^2.0.0", modules = { "cli" } },
}

-- Not knowable when the file is written: a library being developed wants its
-- dependency in devDependencies, a release wants it in dependencies, and the
-- same checkout has to do both. TOML has no way to express "it depends".
local section = daukle.env("DAUKLE_EXAMPLE_DEV") == "1" and "devDependencies"
                                                        or "dependencies"

local dependencies = {}
for index = 1, #WANTED do
  local entry = WANTED[index]
  dependencies[entry.project] = { version = entry.version, modules = entry.modules }
end

daukle.config.consumers = {
  {
    id = "node",
    language = "npm",
    file = "package.json",
    configuration = section,
    dependencies = dependencies,
  },
}

daukle.log("writing " .. #WANTED .. " dependency into " .. section
           .. " for " .. daukle.host.os .. "/" .. daukle.host.arch)
