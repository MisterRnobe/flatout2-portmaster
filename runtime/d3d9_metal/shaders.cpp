#include "../render/commands.h"
#include "../render/terrain_specular.h"
#include "../render/shader_capture.h"
#include "effects.h"
#include "mojoshader.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>
namespace fo2render {
std::vector<uint8_t> withWholeFileCtab(const std::vector<uint8_t> &bytes) {
  uint32_t version = 0;
  std::memcpy(&version, bytes.data(), 4);
  std::vector<uint8_t> d(64, 0);
  auto put32 = [&](size_t at, uint32_t v) {
    std::memcpy(d.data() + at, &v, 4);
  };
  auto put16 = [&](size_t at, uint16_t v) {
    std::memcpy(d.data() + at, &v, 2);
  };
  auto addString = [&](const char *s) {
    const uint32_t at = static_cast<uint32_t>(d.size());
    d.insert(d.end(), s, s + std::strlen(s) + 1);
    return at;
  };
  const uint32_t creator = addString("fo2 port");
  const uint32_t target =
      addString((version & 0xFF00) >= 0x300 ? "vs_3_0" : "vs_2_0");
  const uint32_t name = addString("c");
  while (d.size() % 4)
    d.push_back(0);
  put32(0, 28);
  put32(4, creator);
  put32(8, version);
  put32(12, 1);
  put32(16, 28);
  put32(20, 0);
  put32(24, target);
  put32(28, name);
  put16(32, 2 /* float4 */);
  put16(34, 0);
  put16(36, 256);
  put16(38, 0);
  put32(40, 48);
  put32(44, 0);
  put16(48, 1 /* vector */);
  put16(50, 3 /* float */);
  put16(52, 1);
  put16(54, 4);
  put16(56, 256);
  put16(58, 0);
  put32(60, 0);
  const uint32_t dwords =
      1 + static_cast<uint32_t>(d.size() / 4); // 'CTAB' + the table
  std::vector<uint8_t> out(bytes.begin(), bytes.begin() + 4);
  const uint32_t comment = 0xFFFEu | (dwords << 16),
                 fourcc = 0x42415443u; // "CTAB"
  out.insert(out.end(), reinterpret_cast<const uint8_t *>(&comment),
             reinterpret_cast<const uint8_t *>(&comment) + 4);
  out.insert(out.end(), reinterpret_cast<const uint8_t *>(&fourcc),
             reinterpret_cast<const uint8_t *>(&fourcc) + 4);
  out.insert(out.end(), d.begin(), d.end());
  out.insert(out.end(), bytes.begin() + 4, bytes.end());
  return out;
}

static void appendEntryParameters(std::string &source, const char *entry,
                                  const std::string &parameters) {
  auto function = source.find(std::string(entry) + " (");
  auto opening = source.find('(', function);
  auto closing = source.find(") {", opening);
  if (function == std::string::npos || opening == std::string::npos ||
      closing == std::string::npos)
    throw std::runtime_error("Unrecognized MSL entry signature");
  auto first = source.find_first_not_of(" \t\r\n", opening + 1);
  auto last = source.find_last_not_of(" \t\r\n", closing - 1);
  auto separator = (first == closing || source[last] == ',')
                       ? std::string(" ")
                       : std::string(", ");
  source.insert(closing, separator + parameters + " ");
}

// PS 1.x dependent reads can be expressed as a coordinate move followed by TEX.
// This leaves sampler selection (the destination t register) and source swizzle
// semantics intact while using instructions supported by the unchanged profile.
static Bytes lowerLegacyDependentReads(const Bytes &bytes) {
  uint32_t version;
  std::memcpy(&version, bytes.data(), 4);
  if (version < 0xffff0100u || version >= 0xffff0104u)
    return bytes;
  std::vector<uint32_t> words(bytes.size() / 4);
  std::memcpy(words.data(), bytes.data(), bytes.size());
  std::vector<uint32_t> result{version};
  for (size_t i = 1; i < words.size();) {
    auto op = words[i] & 0xffffu;
    size_t end = i + 1;
    if (op == 0xfffe)
      end += ((words[i] >> 16) & 0x7fff);
    else if (op == 81)
      end += 5; // DEF contains four literal DWORDs, not register tokens.
    else
      while (end < words.size() && (words[end] & 0x80000000u))
        ++end;
    if (end > words.size())
      throw std::runtime_error("Truncated legacy shader instruction");
    if (op == 69 || op == 70 || op == 82) {
      if (end - i != 3)
        throw std::runtime_error("Invalid dependent texture read operands");
      auto source = words[i + 2];
      // Compose .ar/.gb/.rgb with the encoded source swizzle, retaining
      // modifiers.
      uint32_t swizzle = (source >> 16) & 255, composed = 0;
      unsigned components[4] = {op == 69   ? 3u
                                : op == 70 ? 1u
                                           : 0u,
                                op == 69   ? 0u
                                : op == 70 ? 2u
                                           : 1u,
                                op == 82 ? 2u : 0u, 3u};
      for (unsigned n = 0; n < 4; ++n)
        composed |= ((swizzle >> (components[n] * 2)) & 3) << (n * 2);
      source = (source & ~0x00ff0000u) | (composed << 16);
      result.insert(result.end(),
                    {1u, words[i + 1], source, 66u, words[i + 1]}); // MOV; TEX
    } else
      result.insert(result.end(), words.begin() + i, words.begin() + end);
    i = end;
  }
  Bytes out(result.size() * 4);
  std::memcpy(out.data(), result.data(), out.size());
  return out;
}

// The Metal profile uses a 256-byte expression buffer for matrix instructions.
// Relative constant accesses can overflow it. Emit scalar dot instructions
// instead; snapshot an aliased source before writing any destination component.
static Bytes lowerVertexMatrices(const Bytes &bytes) {
  uint32_t version;
  std::memcpy(&version, bytes.data(), 4);
  if (version != 0xfffe0101u)
    return bytes;
  std::vector<uint32_t> words(bytes.size() / 4);
  std::memcpy(words.data(), bytes.data(), bytes.size());
  std::vector<std::pair<size_t, size_t>> instructions;
  std::array<bool, 12> used{};
  auto type = [](uint32_t token) {
    return ((token >> 28) & 7) | ((token >> 8) & 24);
  };
  auto rows = [](uint32_t op) {
    return op == 20 || op == 22 ? 4u : op == 24 ? 2u : 3u;
  };
  for (size_t i = 1; i < words.size();) {
    auto op = words[i] & 0xffffu;
    size_t end = i + 1;
    if (op == 0xfffe)
      end += ((words[i] >> 16) & 0x7fff);
    else if (op == 81)
      end += 5;
    else
      while (end < words.size() && (words[end] & 0x80000000u))
        ++end;
    if (end > words.size())
      throw std::runtime_error("Truncated vertex instruction");
    if (op != 0xfffe) {
      for (size_t j = i + 1; j < end && (op != 81 || j == i + 1); ++j)
        if (type(words[j]) == 0 && (words[j] & 2047) < used.size())
          used[words[j] & 2047] = true;
      if (op >= 20 && op <= 24 && end - i == 4 && type(words[i + 3]) == 0)
        for (unsigned n = 0; n < rows(op); ++n) {
          auto r = (words[i + 3] & 2047) + n;
          if (r < used.size())
            used[r] = true;
        }
    }
    instructions.push_back({i, end});
    i = end;
  }
  unsigned scratch = 0;
  while (scratch < used.size() && used[scratch])
    ++scratch;
  std::vector<uint32_t> result{version};
  for (auto range : instructions) {
    auto i = range.first, end = range.second, op = size_t(words[i] & 0xffffu);
    if (op < 20 || op > 24) {
      result.insert(result.end(), words.begin() + i, words.begin() + end);
      continue;
    }
    if (end - i != 4)
      throw std::runtime_error("Invalid vertex matrix operands");
    auto dest = words[i + 1], source = words[i + 2], matrix = words[i + 3];
    const bool aliased =
        type(dest) == type(source) && (dest & 2047) == (source & 2047);
    if (aliased) {
      if (scratch == used.size())
        throw std::runtime_error(
            "Aliased vertex matrix needs a free temporary");
      result.insert(result.end(), {1u, 0x800f0000u | scratch, source});
      source = 0x80e40000u | scratch;
    }
    for (unsigned row = 0; row < rows(op); ++row) {
      auto mask = 1u << (16 + row);
      if (!(dest & mask))
        continue;
      auto index = (matrix & 2047) + row;
      if (index > 2047)
        throw std::runtime_error("Matrix constant register overflow");
      result.insert(result.end(), {op == 20 || op == 21 ? 9u : 8u,
                                   (dest & ~0x000f0000u) | mask, source,
                                   (matrix & ~2047u) | index});
    }
  }
  Bytes resultBytes(result.size() * 4);
  std::memcpy(resultBytes.data(), result.data(), resultBytes.size());
  return resultBytes;
}

static void validateMslDelimiters(const std::string &source) {
  std::string stack;
  bool line = false, block = false;
  for (size_t i = 0; i < source.size(); ++i) {
    char c = source[i], next = i + 1 < source.size() ? source[i + 1] : 0;
    if (line) {
      if (c == '\n')
        line = false;
      continue;
    }
    if (block) {
      if (c == '*' && next == '/') {
        block = false;
        ++i;
      }
      continue;
    }
    if (c == '/' && next == '/') {
      line = true;
      ++i;
      continue;
    }
    if (c == '/' && next == '*') {
      block = true;
      ++i;
      continue;
    }
    if (c == '(' || c == '[' || c == '{')
      stack += c;
    else if (c == ')' || c == ']' || c == '}') {
      char open = c == ')' ? '(' : c == ']' ? '[' : '{';
      if (stack.empty() || stack.back() != open)
        throw std::runtime_error("Truncated or unbalanced generated MSL");
      stack.pop_back();
    }
  }
  if (!stack.empty() || block)
    throw std::runtime_error("Incomplete generated MSL");
}
static void repairLegacyTextureInputs(std::string &source, const char *entry) {
  const std::regex initial(R"(float4 (t([0-9]+)) = input\.\1;)");
  struct Input {
    std::string name;
    unsigned index;
  };
  std::vector<Input> inputs;
  for (auto i = std::sregex_iterator(source.begin(), source.end(), initial);
       i != std::sregex_iterator(); ++i)
    inputs.push_back({(*i)[1], unsigned(std::stoul((*i)[2]))});
  for (auto &input : inputs) {
    const std::string declaration =
        "float4 " + input.name + " = input." + input.name + ";";
    auto initialAt = source.find(declaration);
    auto after = initialAt + declaration.size();
    std::smatch first;
    auto rest = source.substr(after);
    // PS 1.1 has straight-line code. A full write before the first read makes
    // the initial interpolator unnecessary (including lowered TEXREG2*
    // targets).
    if (std::regex_search(rest, first,
                          std::regex("\\b" + input.name + "\\b"))) {
      auto tail = rest.substr(first.position() + input.name.size());
      std::smatch assignment;
      if (std::regex_search(tail, assignment, std::regex(R"(^\s*=([^;]*);)")) &&
          !std::regex_search(assignment[1].str(),
                             std::regex("\\b" + input.name + "\\b"))) {
        source.replace(initialAt, declaration.size(),
                       "float4 " + input.name + ";");
        continue;
      }
    }
    auto structure = source.find("struct fo2_fragment_Input");
    if (structure == std::string::npos) {
      auto function = source.find("fragment fo2_fragment_Output");
      source.insert(function, "struct fo2_fragment_Input {\n};\n");
      appendEntryParameters(source, entry,
                            "fo2_fragment_Input input [[stage_in]]");
      structure = source.find("struct fo2_fragment_Input");
    }
    auto begin = source.find('{', structure), end = source.find("};", begin);
    auto members = source.substr(begin, end - begin);
    if (!std::regex_search(members,
                           std::regex("\\bfloat4\\s+" + input.name + "\\b")))
      source.insert(begin + 1, "\nfloat4 " + input.name + " [[user(texcoord" +
                                   std::to_string(input.index) + ")]];\n");
  }
}

static void validateEntryParameters(const std::string &source,
                                    const char *entry) {
  auto opening = source.find('(', source.find(std::string(entry) + " ("));
  if (opening == std::string::npos)
    throw std::runtime_error("Missing MSL entry parameters");
  unsigned depth = 1, brackets = 0;
  bool argument = false;
  for (size_t i = opening + 1; i < source.size(); ++i) {
    char c = source[i];
    if (c == ')' && depth == 1) {
      if (!argument)
        throw std::runtime_error("Empty MSL entry argument");
      return;
    }
    if (c == ',' && depth == 1 && !brackets) {
      if (!argument)
        throw std::runtime_error("Duplicate MSL argument separator");
      argument = false;
      continue;
    }
    if (c == '(')
      ++depth;
    else if (c == ')')
      --depth;
    else if (c == '[')
      ++brackets;
    else if (c == ']')
      --brackets;
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
      argument = true;
  }
  throw std::runtime_error("Unterminated MSL entry parameters");
}

static void validateInputReferences(const std::string &source,
                                    const char *entry) {
  const auto structure = source.find("struct " + std::string(entry) + "_Input");
  std::string members;
  if (structure != std::string::npos) {
    auto begin = source.find('{', structure), end = source.find("};", begin);
    members = source.substr(begin, end - begin);
  }
  const std::regex reference(R"(\binput\.([a-zA-Z0-9_]+))");
  for (auto i = std::sregex_iterator(source.begin(), source.end(), reference);
       i != std::sregex_iterator(); ++i) {
    auto name = (*i)[1].str();
    if (!std::regex_search(
            members,
            std::regex("\\b(?:float[1-4]?|bool|int[1-4]?)\\s+" + name + "\\b")))
      throw std::runtime_error("MSL references undeclared input " + name);
  }
}

std::shared_ptr<Shader> translateShader(const Bytes &bytes, bool vertex,
                                        uint32_t colorMask, uint32_t cubeMask,
                                        bool vertexFog) {
  if (bytes.size() < 8 || bytes.size() % 4)
    throw std::runtime_error("Invalid D3D shader bytecode size");
  captureShader(bytes.data(), bytes.size());
  MOJOSHADER_samplerMap samplers[16];
  for (unsigned i = 0; i < 16; ++i) {
    samplers[i].index = i;
    samplers[i].type = (cubeMask & (1u << i)) ? MOJOSHADER_SAMPLER_CUBE
                                              : MOJOSHADER_SAMPLER_2D;
  }
  uint32_t shaderVersion;
  std::memcpy(&shaderVersion, bytes.data(), 4);
  const bool samplerOverride = !vertex && ((shaderVersion >> 8) & 255) < 2;
  const char *entry = vertex ? "fo2_vertex" : "fo2_fragment";
  auto parse = [&](const Bytes &b) {
    return MOJOSHADER_parse(MOJOSHADER_PROFILE_METAL, entry, b.data(), b.size(),
                            nullptr, 0, samplers, samplerOverride ? 16 : 0,
                            nullptr, nullptr, nullptr);
  };
  const Bytes lowered = lowerVertexMatrices(lowerLegacyDependentReads(bytes));
  const MOJOSHADER_parseData *data = parse(lowered);
  if (data && data->error_count && vertex) {
    bool missing = false;
    for (int i = 0; i < data->error_count; ++i)
      if (data->errors[i].error &&
          std::strstr(data->errors[i].error, "without a CTAB"))
        missing = true;
    if (missing) {
      MOJOSHADER_freeParseData(data);
      data = parse(withWholeFileCtab(lowered));
    }
  }
  if (!data)
    throw std::runtime_error("MojoShader Metal parser allocation failed");
  if (data->error_count) {
    std::string error;
    for (int i = 0; i < data->error_count; ++i) {
      error += data->errors[i].error;
      error += '\n';
    }
    MOJOSHADER_freeParseData(data);
    throw std::runtime_error("D3D bytecode -> MSL failed: " + error);
  }
  auto shader = std::make_shared<Shader>();
  shader->vertex = vertex;
  const auto originalHash=shaderHash(bytes.data(),bytes.size());
  shader->physicalWaterFamily=vertex?
      (originalHash==0x301fd29e4853a13cull&&bytes.size()==388?1:originalHash==0x889e687f1752525aull&&bytes.size()==408?2:0):
      (originalHash==0xb20adcd636eb7f43ull&&bytes.size()==128&&(cubeMask&2)?1:
       originalHash==0x40e19e2958b0afb1ull&&bytes.size()==60&&!(cubeMask&3)?2:0);
  shader->terrainSpecularMask = !vertex && terrainSpecularProgram(shaderHash(bytes.data(),bytes.size()),bytes.size()) && !(cubeMask&3);
  shader->entry = entry;
  shader->source = std::string("#include <metal_stdlib>\n") +
                   (data->output ? data->output : "");
  shader->hash = shaderHash(bytes.data(), bytes.size()) ^ colorMask ^
                 (uint64_t(cubeMask) << 32);
  for (int i = 0; i < data->uniform_count; ++i) {
    auto &u = data->uniforms[i];
    if (u.constant)
      continue;
    uint32_t count = u.array_count ? u.array_count : 1;
    shader->uniforms.push_back({uint32_t(u.type), uint32_t(u.index), count});
    if (u.type == MOJOSHADER_UNIFORM_FLOAT)
      shader->floats += count;
    else if (u.type == MOJOSHADER_UNIFORM_INT)
      shader->ints += count;
    else
      shader->bools += count;
  }
  for (int i = 0; i < data->sampler_count; ++i) {
    auto &sampler = data->samplers[i];
    if (sampler.index < 0 || sampler.index >= 16)
      throw std::runtime_error("Unsupported Metal sampler slot");
    shader->samplerMask |= 1u << sampler.index;
    if (sampler.type == MOJOSHADER_SAMPLER_CUBE)
      shader->cubeSamplerMask |= 1u << sampler.index;
  }
  for (int i = 0; i < data->attribute_count; ++i) {
    auto &a = data->attributes[i];
    std::string name = a.name ? a.name : "";
    std::smatch match;
    uint32_t slot = i;
    if (std::regex_search(name, match, std::regex("v([0-9]+)$")))
      slot = std::stoul(match[1]);
    shader->inputs.push_back({slot, uint32_t(a.usage), uint32_t(a.index)});
    if (vertex && (colorMask & (1u << slot))) {
      std::regex input("input\\.v" + std::to_string(slot) + "\\b");
      shader->source = std::regex_replace(
          shader->source, input, "(input.v" + std::to_string(slot) + ".zyxw)");
    }
  }
  MOJOSHADER_freeParseData(data);
  shader->source = std::regex_replace(
      shader->source,
      std::regex(
          R"((#define[ \t]+[a-zA-Z_][a-zA-Z0-9_]*[ \t]+uniforms\.[^;\r\n]+);)"),
      "$1");
  if (!vertex && shaderVersion >= 0xffff0100u && shaderVersion < 0xffff0104u)
    repairLegacyTextureInputs(shader->source, entry);
  if (vertex) {
    std::smatch match;
    if (!std::regex_search(
            shader->source, match,
            std::regex(R"(float4 ([a-zA-Z0-9_]+) \[\[position\]\])")))
      throw std::runtime_error("Missing Metal vertex position");
    auto position = "output." + match[1].str();
    appendEntryParameters(shader->source, entry,
                          "constant float* fo2fx [[buffer(30)]]");
    shader->source.insert(shader->source.rfind("return output;"),
                          position + ".xy += float2(fo2fx[20],fo2fx[21])*" +
                              position + ".w;\n");
  }
  if (!vertex) {
    // D3D9 alpha testing is fixed pipeline state, including programmable
    // pixels.
    std::string position = "fo2position";
    std::smatch match;
    if (std::regex_search(
            shader->source, match,
            std::regex(R"(float4 ([a-zA-Z0-9_]+) \[\[position\]\])")))
      position = "input." + match[1].str();
    std::string args = "constant float* fo2fx [[buffer(30)]] ";
    if (position == "fo2position")
      args += ", float4 fo2position [[position]] ";
    appendEntryParameters(shader->source, entry, args);
    const std::string ret = "return output;";
    size_t at = shader->source.rfind(ret);
    if (at == std::string::npos)
      throw std::runtime_error("Missing MSL output");
    std::string fog = "1.0";
    std::smatch fogMatch;
    if (std::regex_search(
            shader->source, fogMatch,
            std::regex(R"(float4 ([a-zA-Z0-9_]+) \[\[user\(fog\)\]\])")))
      fog = "input." + fogMatch[1].str() + ".x";
    else if (vertexFog) {
      const std::string name = "struct fo2_fragment_Input";
      auto input = shader->source.find(name);
      if (input != std::string::npos) {
        auto brace = shader->source.find('{', input);
        shader->source.insert(brace + 1, "\nfloat4 fo2fog [[user(fog)]];\n");
      } else {
        auto function = shader->source.find("fragment fo2_fragment_Output");
        shader->source.insert(
            function,
            "struct fo2_fragment_Input {float4 fo2fog [[user(fog)]];};\n");
        auto arguments = shader->source.find(
            '(', shader->source.find(std::string(entry) + " ("));
        shader->source.insert(arguments + 1,
                              "fo2_fragment_Input input [[stage_in]],");
      }
      fog = "input.fo2fog.x";
    }
    at = shader->source.rfind(ret);
    uint32_t version;
    std::memcpy(&version, bytes.data(), 4);
    if (shader->source.find("oC0") != std::string::npos)
      shader->source.insert(at, pixelEffects("output.oC0", position, fog,
                                             ((version >> 8) & 255) < 3));
  }
  if (!vertex) {
    // Add sampler LOD bias only to implicit-derivative samples; explicit
    // level/gradient samples keep their operands.
    size_t at = 0;
    while ((at = shader->source.find(".sample(", at)) != std::string::npos) {
      size_t begin = at + 8, end = begin;
      unsigned depth = 1, commas = 0;
      for (; end < shader->source.size(); ++end) {
        char c = shader->source[end];
        if (c == '(')
          ++depth;
        else if (c == ')') {
          if (!--depth)
            break;
        } else if (c == ',' && depth == 1)
          ++commas;
      }
      std::smatch sampler;
      auto args = shader->source.substr(begin, end - begin);
      if (commas == 1 &&
          std::regex_search(args, sampler,
                            std::regex(R"(^\s*s([0-9]+)\s*,)"))) {
        auto stage = std::stoul(sampler[1]);
        if (stage < 16) {
          auto bias = ",bias(fo2fx[" + std::to_string(32 + stage) + "])";
          shader->source.insert(end, bias);
          end += bias.size();
        }
      }
      at = end + 1;
    }
  }
  validateMslDelimiters(shader->source);
  validateEntryParameters(shader->source, entry);
  validateInputReferences(shader->source, entry);
  shader->hash = shaderHash(shader->source.data(), shader->source.size());
  return shader;
}
Bytes packConstants(const Shader &shader, const float *floats,
                    const int32_t *ints, const uint32_t *bools) {
  Bytes result(((shader.floats + shader.ints) * 16 + shader.bools + 15) &
                   ~size_t(15),
               0);
  size_t f = 0, i = shader.floats * 16, b = (shader.floats + shader.ints) * 16;
  for (auto &u : shader.uniforms) {
    if (u.kind == MOJOSHADER_UNIFORM_FLOAT) {
      if (u.index + u.count > 256)
        throw std::runtime_error("Float uniform register overflow");
      std::memcpy(result.data() + f, floats + u.index * 4, u.count * 16);
      f += u.count * 16;
    } else if (u.kind == MOJOSHADER_UNIFORM_INT) {
      if (u.index + u.count > 16)
        throw std::runtime_error("Integer uniform overflow");
      std::memcpy(result.data() + i, ints + u.index * 4, u.count * 16);
      i += u.count * 16;
    } else {
      if (u.index + u.count > 16)
        throw std::runtime_error("Boolean uniform overflow");
      for (unsigned j = 0; j < u.count; ++j)
        result[b++] = bools[u.index + j] ? 1 : 0;
    }
  }
  return result;
}
} // namespace fo2render
