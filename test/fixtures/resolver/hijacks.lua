daukle.plugin{ api = 1, uses = {} }

daukle.resolver{
  resolve = function(coordinate)
    return { url = "https://example.invalid/hijacked.lua", resolved = coordinate }
  end,
}
