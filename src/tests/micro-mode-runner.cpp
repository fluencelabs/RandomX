/*
Copyright (c) 2018-2019, tevador <tevador@gmail.com>

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
	* Redistributions of source code must retain the above copyright
	  notice, this list of conditions and the following disclaimer.
	* Redistributions in binary form must reproduce the above copyright
	  notice, this list of conditions and the following disclaimer in the
	  documentation and/or other materials provided with the distribution.
	* Neither the name of the copyright holder nor the
	  names of its contributors may be used to endorse or promote products
	  derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <fstream>
#include <iostream>
#include <iomanip>
#include <exception>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include "stopwatch.hpp"
#include "utility.hpp"
#include "../randomx.h"
#include "../dataset.hpp"
#include "../blake2/endian.h"
#include "../common.hpp"
#include "../jit_compiler.hpp"
#ifdef _WIN32
#include <windows.h>
#include <versionhelpers.h>
#endif
#include "affinity.hpp"

const uint8_t blockTemplate_[] = {
		0x07, 0x07, 0xf7, 0xa4, 0xf0, 0xd6, 0x05, 0xb3, 0x03, 0x26, 0x08, 0x16, 0xba, 0x3f, 0x10, 0x90, 0x2e, 0x1a, 0x14,
		0x5a, 0xc5, 0xfa, 0xd3, 0xaa, 0x3a, 0xf6, 0xea, 0x44, 0xc1, 0x18, 0x69, 0xdc, 0x4f, 0x85, 0x3f, 0x00, 0x2b, 0x2e,
		0xea, 0x00, 0x00, 0x00, 0x00, 0x77, 0xb2, 0x06, 0xa0, 0x2c, 0xa5, 0xb1, 0xd4, 0xce, 0x6b, 0xbf, 0xdf, 0x0a, 0xca,
		0xc3, 0x8b, 0xde, 0xd3, 0x4d, 0x2d, 0xcd, 0xee, 0xf9, 0x5c, 0xd2, 0x0c, 0xef, 0xc1, 0x2f, 0x61, 0xd5, 0x61, 0x09
};

class AtomicHash {
public:
	AtomicHash() {
		for (int i = 0; i < 4; ++i)
			hash[i].store(0);
	}
	void xorWith(uint64_t update[4]) {
		for (int i = 0; i < 4; ++i)
			hash[i].fetch_xor(update[i]);
	}
	void print(std::ostream& os) {
		for (int i = 0; i < 4; ++i)
			print(hash[i], os);
		os << std::endl;
	}
	static void print(std::atomic<uint64_t>& hash, std::ostream& os) {
		auto h = hash.load();
		outputHex(std::cout, (char*)&h, sizeof(h));
	}
	std::atomic<uint64_t> hash[4];
};

void printUsage(const char* executable) {
	std::cout << "Usage: " << executable << " [OPTIONS]" << std::endl;
	std::cout << "Supported options:" << std::endl;
	std::cout << "  --help        shows this message" << std::endl;
	std::cout << "  --mine        mining mode: 2080 MiB" << std::endl;
	std::cout << "  --verify      verification mode: 256 MiB" << std::endl;
	std::cout << "  --jit         JIT compiled mode (default: interpreter)" << std::endl;
	std::cout << "  --secure      W^X policy for JIT pages (default: off)" << std::endl;
	std::cout << "  --largePages  use large pages (default: small pages)" << std::endl;
	std::cout << "  --softAes     use software AES (default: hardware AES)" << std::endl;
	std::cout << "  --threads T   use T threads (default: 1)" << std::endl;
	std::cout << "  --affinity A  thread affinity bitmask (default: 0)" << std::endl;
	std::cout << "  --init Q      initialize dataset with Q threads (default: 1)" << std::endl;
	std::cout << "  --nonces N    run N nonces (default: 1000)" << std::endl;
	std::cout << "  --seed S      seed for cache initialization (default: 0)" << std::endl;
	std::cout << "  --ssse3       use optimized Argon2 for SSSE3 CPUs" << std::endl;
	std::cout << "  --avx2        use optimized Argon2 for AVX2 CPUs" << std::endl;
	std::cout << "  --auto        select the best options for the current CPU" << std::endl;
	std::cout << "  --noBatch     calculate hashes one by one (default: batch)" << std::endl;
	std::cout << "  --commit      calculate commitments instead of hashes (default: hashes)" << std::endl;
}

struct MemoryException : public std::exception {
};
struct CacheAllocException : public MemoryException {
	const char * what() const throw () {
		return "Cache allocation failed";
	}
};
struct DatasetAllocException : public MemoryException {
	const char * what() const throw () {
		return "Dataset allocation failed";
	}
};

using MineFunc = void(randomx_vm * vm, std::atomic<uint32_t> & atomicNonce, AtomicHash & result, uint32_t noncesCount, int thread, int cpuid);

template<bool batch, bool commit>
void mine(randomx_vm* vm, std::atomic<uint32_t>& atomicNonce, AtomicHash& result, uint32_t noncesCount, int thread, int cpuid = -1) {
	if (cpuid >= 0) {
		int rc = set_thread_affinity(cpuid);
		if (rc) {
			std::cerr << "Failed to set thread affinity for thread " << thread << " (error=" << rc << ")" << std::endl;
		}
	}
	uint64_t hash[RANDOMX_HASH_SIZE / sizeof(uint64_t)];
	uint8_t blockTemplate[sizeof(blockTemplate_)];
	memcpy(blockTemplate, blockTemplate_, sizeof(blockTemplate));
	void* noncePtr = blockTemplate + 39;
	auto nonce = atomicNonce.fetch_add(1);

	if (batch) {
		store32(noncePtr, nonce);
		randomx_calculate_hash_first(vm, blockTemplate, sizeof(blockTemplate));
	}

	while (nonce < noncesCount) {
		if (batch) {
			nonce = atomicNonce.fetch_add(1);
		}
		store32(noncePtr, nonce);
		(batch ? randomx_calculate_hash_next : randomx_calculate_hash)(vm, blockTemplate, sizeof(blockTemplate), &hash);
		if (commit) {
			randomx_calculate_commitment(blockTemplate, sizeof(blockTemplate), &hash, &hash);
		}
		result.xorWith(hash);
		if (!batch) {
			nonce = atomicNonce.fetch_add(1);
		}
	}
}

int main(int argc, char** argv) {
	bool softAes, miningMode, verificationMode, help, largePages, jit, secure, commit;
	bool ssse3, avx2, autoFlags, noBatch;
	int noncesCount, threadCount, initThreadCount;
	uint64_t threadAffinity;
	int32_t seedValue;
	char seed[4];

	readOption("--softAes", argc, argv, softAes);
	readOption("--mine", argc, argv, miningMode);
	readOption("--verify", argc, argv, verificationMode);
	readIntOption("--threads", argc, argv, threadCount, 1);
	readUInt64Option("--affinity", argc, argv, threadAffinity, 0);
	readIntOption("--nonces", argc, argv, noncesCount, 1000);
	readIntOption("--init", argc, argv, initThreadCount, 1);
	readIntOption("--seed", argc, argv, seedValue, 0);
	readOption("--largePages", argc, argv, largePages);
	if (!largePages) {
		readOption("--largepages", argc, argv, largePages);
	}
	readOption("--jit", argc, argv, jit);
	readOption("--help", argc, argv, help);
	readOption("--secure", argc, argv, secure);
	readOption("--ssse3", argc, argv, ssse3);
	readOption("--avx2", argc, argv, avx2);
	readOption("--auto", argc, argv, autoFlags);
	readOption("--noBatch", argc, argv, noBatch);
	readOption("--commit", argc, argv, commit);

	store32(&seed, seedValue);


	if (help) {
		printUsage(argv[0]);
		return 0;
	}

	randomx_vm* vm;
	randomx_cache* cache;
	randomx_flags flags = RANDOMX_FLAG_DEFAULT;



	try {
          AtomicHash hash;
          cache = randomx_alloc_cache(flags);
          randomx_init_cache(cache, &seed, sizeof(seed));
          uint8_t microCache[RANDOMX_PROGRAM_COUNT * RANDOMX_PROGRAM_ITERATIONS * randomx::CacheLineSize];
          cache->microCache = &microCache[0];
          randomx_vm *vm = randomx_create_vm(flags, cache, nullptr);
          uint8_t result[RANDOMX_HASH_SIZE];
          randomx_calculate_hash(vm, &blockTemplate_, sizeof(blockTemplate_), &hash.hash[0]);
          randomx_destroy_vm(vm);
          randomx_release_cache(cache);

          std::cout << "Hash light: ";
          hash.print(std::cout);

          randomx_flags micro_flags = RANDOMX_FLAG_MICRO;

          randomx_cache* micro_cache = randomx_alloc_cache(micro_flags);
          cache->microCache = &microCache[0];

          randomx_init_cache(cache, &seed, sizeof(seed));
          randomx_vm *micro_vm = randomx_create_micro_vm(micro_flags, cache, nullptr);
          randomx_calculate_hash(micro_vm, &blockTemplate_,
                                 sizeof(blockTemplate_), cache);
          randomx_destroy_vm(micro_vm);
          std::cout << "Hash micro: ";
          hash.print(std::cout);

        // crashes
          randomx_release_cache(micro_cache);

    }
	catch (std::exception& e) {
		std::cout << "ERROR: " << e.what() << std::endl;
		return 1;
	}
	return 0;
}
