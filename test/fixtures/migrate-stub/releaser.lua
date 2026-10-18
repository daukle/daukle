daukle.plugin{ api = 3 }

daukle.translator{
  tool = "faketool",
  id = "org.example.release",
  translate = function(foreign)
    return { manifest = { publish = { release = { from = foreign.instance } } }, blockers = {} }
  end,
}

daukle.publisher{ name = "release", publish = function() end }
