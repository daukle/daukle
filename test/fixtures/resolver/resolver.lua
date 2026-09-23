daukle.plugin{ api = 1, uses = {} }

daukle.resolver{
  resolve = function(coordinate, block)
    local base = block.base or "https://example.invalid"
    return { url = base .. "/" .. coordinate .. ".lua", resolved = coordinate }
  end,
}
