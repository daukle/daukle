daukle.plugin{ api = 3, uses = { "exec", "tool", "write" } }

daukle.toolchain{ name = "stub", generate = function() return {} end }

daukle.migrator{
  tool = "faketool",
  toolchain = "stub",
  run = function(context)
    local path = daukle.write{ path = "probe.txt", text = context.instance .. " " .. context.root }
    return {
      manifest = { toolchains = { [context.instance] = { version = "1" } } },
      foreign = { { kind = "plugins", id = "org.example.release", config = { draft = true } } },
      blockers = {},
      owned = { "faketool.conf" },
      proof = { task = "stub:build", compare = { { old = "out/a.txt", new = "build/daukle/stub/a.txt", as = "bytes" } } },
      wrote = path ~= nil,
    }
  end,
  build = function(context)
    daukle.write{ path = "built.txt", text = context.proof.task }
  end,
}

daukle.translator{
  tool = "faketool",
  id = "org.example.release",
  translate = function(foreign)
    return { manifest = { publish = { release = { from = foreign.instance, draft = foreign.config.draft } } }, blockers = {} }
  end,
}
