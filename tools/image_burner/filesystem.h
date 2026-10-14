// Copyright 2026 The Kemu Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "common.h"
namespace kemu {
struct Replacement { std::string path, source, sha256; };
void SetFilesystemTool(const std::string& path);
uint64_t WorkingFilesystemSize(const std::string& image, const std::vector<Replacement>& files);
void ReplaceFiles(const std::string& image, const std::string& partition,
                  const std::vector<Replacement>& files);
}  // namespace kemu
