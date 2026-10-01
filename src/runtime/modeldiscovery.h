#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace kestrel::runtime {

// One thing on disk that might be a model, and how big it is.
//
// The size is carried rather than recomputed by the caller because it is what
// the ranking is: a model is chosen by how many bytes it is, and the caller
// only needs the order.
struct ModelCandidate {
    std::string path;
    std::size_t bytes = 0;
};

// Model candidates under `searchDirectory`, most bytes first.
//
// Two layouts, because there are two real backends reading two model formats: a
// .gguf *file* for llama.cpp, and a *directory* holding genai_config.json for
// ONNX Runtime GenAI. A GenAI model is a config plus one or more ONNX graphs
// plus external data, so it is a folder rather than a file -- and requiring a
// .gguf here is how the ONNX backend was once unreachable from a plain launch.
//
// Largest first, because a quantisation of the same family differs in size by a
// wide margin and among different families the larger model is the more capable
// one. Kestrel shipped a `qwen.gguf` beside a much better local model precisely
// because the name said nothing about the contents. Ties break on path so the
// same directory always yields the same order.
//
// A *list*, not a winner. Size says who to try; only loading a model settles
// whether it works, and a folder can hold gigabytes of weights behind a config
// that names a graph nobody shipped. The caller walks the list until one loads.
//
// This lives here rather than in main.cpp because it is a decision about the
// filesystem, it is the same kind of thing backendregistry decides, and a rule
// that can only be reached from a GUI entry point cannot be tested. That last
// point is not theoretical: the ranking shipped unexercised, and the retry that
// was supposed to make a failed first guess survivable was wired to a signal in
// the wrong order and never ran at all.
[[nodiscard]] std::vector<ModelCandidate> discoverModels(
    const std::filesystem::path& searchDirectory);

} // namespace kestrel::runtime
