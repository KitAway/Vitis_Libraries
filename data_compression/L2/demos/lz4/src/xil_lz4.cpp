/*
 * (c) Copyright 2019 Xilinx, Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
#include "xil_lz4.hpp"
#include "xxhash.h"

#define BLOCK_SIZE 64
#define KB 1024
#define MAGIC_HEADER_SIZE 4
#define MAGIC_BYTE_1 4
#define MAGIC_BYTE_2 34
#define MAGIC_BYTE_3 77
#define MAGIC_BYTE_4 24
#define FLG_BYTE 104

uint64_t xfLz4::getEventDurationNs(const cl::Event& event) {
    uint64_t start_time = 0, end_time = 0;

    event.getProfilingInfo<uint64_t>(CL_PROFILING_COMMAND_START, &start_time);
    event.getProfilingInfo<uint64_t>(CL_PROFILING_COMMAND_END, &end_time);
    return (end_time - start_time);
}

uint64_t xfLz4::compressFile(std::string& inFile_name, std::string& outFile_name, uint64_t input_size) {
    if (m_switch_flow == 0) { // Xilinx FPGA compression flow
        std::ifstream inFile(inFile_name.c_str(), std::ifstream::binary);
        std::ofstream outFile(outFile_name.c_str(), std::ofstream::binary);

        if (!inFile) {
            std::cout << "Unable to open file";
            exit(1);
        }

        std::vector<uint8_t, aligned_allocator<uint8_t> > in(input_size);
        std::vector<uint8_t, aligned_allocator<uint8_t> > out(input_size);

        inFile.read((char*)in.data(), input_size);

        // LZ4 header
        outFile.put(MAGIC_BYTE_1);
        outFile.put(MAGIC_BYTE_2);
        outFile.put(MAGIC_BYTE_3);
        outFile.put(MAGIC_BYTE_4);

        // FLG & BD bytes
        // --no-frame-crc flow
        // --content-size
        outFile.put(FLG_BYTE);

        // Default value 64K
        uint8_t block_size_header = 0;
        switch (m_block_size_in_kb) {
            case 64:
                outFile.put(BSIZE_STD_64KB);
                block_size_header = BSIZE_STD_64KB;
                break;
            case 256:
                outFile.put(BSIZE_STD_256KB);
                block_size_header = BSIZE_STD_256KB;
                break;
            case 1024:
                outFile.put(BSIZE_STD_1024KB);
                block_size_header = BSIZE_STD_1024KB;
                break;
            case 4096:
                outFile.put(BSIZE_STD_4096KB);
                block_size_header = BSIZE_STD_4096KB;
                break;
            default:
                std::cout << "Invalid Block Size" << std::endl;
                break;
        }

        uint32_t host_buffer_size = (m_block_size_in_kb * 1024) * 32;

        if ((m_block_size_in_kb * 1024) > input_size) host_buffer_size = m_block_size_in_kb * 1024;

        uint8_t temp_buff[10] = {FLG_BYTE,         block_size_header, input_size,       input_size >> 8,
                                 input_size >> 16, input_size >> 24,  input_size >> 32, input_size >> 40,
                                 input_size >> 48, input_size >> 56};

        // xxhash is used to calculate hash value
        uint32_t xxh = XXH32(temp_buff, 10, 0);
        uint64_t enbytes;
        outFile.write((char*)&temp_buff[2], 8);

        // Header CRC
        outFile.put((uint8_t)(xxh >> 8));
        // LZ4 multiple/single cu sequential version
        enbytes = compressSequential(in.data(), out.data(), input_size, host_buffer_size);
        // Writing compressed data
        outFile.write((char*)out.data(), enbytes);

        outFile.put(0);
        outFile.put(0);
        outFile.put(0);
        outFile.put(0);

        // Close file
        inFile.close();
        outFile.close();
        return enbytes;
    } else { // Standard LZ4 flow
        std::string command = "../../../common/lz4/lz4 --content-size -f -q " + inFile_name;
        system(command.c_str());
        std::string output = inFile_name + ".lz4";
        std::string rout = inFile_name + ".std.lz4";
        std::string rename = "mv " + output + " " + rout;
        system(rename.c_str());
        return 0;
    }
}

int validate(std::string& inFile_name, std::string& outFile_name) {
    std::string command = "cmp " + inFile_name + " " + outFile_name;
    int ret = system(command.c_str());
    return ret;
}

// Constructor
xfLz4::xfLz4() {
    for (uint32_t i = 0; i < MAX_COMPUTE_UNITS; i++) {
        for (uint32_t j = 0; j < OVERLAP_BUF_COUNT; j++) {
            // Index calculation
            h_buf_in[i][j].resize(HOST_BUFFER_SIZE);
            h_buf_out[i][j].resize(HOST_BUFFER_SIZE);
            h_blksize[i][j].resize(MAX_NUMBER_BLOCKS);
            h_compressSize[i][j].resize(MAX_NUMBER_BLOCKS);

            m_compressSize[i][j].reserve(MAX_NUMBER_BLOCKS);
            m_blkSize[i][j].reserve(MAX_NUMBER_BLOCKS);
        }
    }
}

// Destructor
xfLz4::~xfLz4() {}

int xfLz4::init(const std::string& binaryFile) {
    // unsigned fileBufSize;
    // The get_xil_devices will return vector of Xilinx Devices
    std::vector<cl::Device> devices = xcl::get_xil_devices();
    cl::Device device = devices[0];

    // Creating Context and Command Queue for selected Device
    m_context = new cl::Context(device);
    m_q = new cl::CommandQueue(*m_context, device, CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE | CL_QUEUE_PROFILING_ENABLE);
    std::string device_name = device.getInfo<CL_DEVICE_NAME>();
    std::cout << "Found Device=" << device_name.c_str() << std::endl;

    // import_binary() command will find the OpenCL binary file created using the
    // xocc compiler load into OpenCL Binary and return as Binaries
    // OpenCL and it can contain many functions which can be executed on the
    // device.
    auto fileBuf = xcl::read_binary_file(binaryFile);
    cl::Program::Binaries bins{{fileBuf.data(), fileBuf.size()}};
    devices.resize(1);

    m_program = new cl::Program(*m_context, devices, bins);

    if (SINGLE_XCLBIN) {
        // Create Compress kernels
        for (uint32_t i = 0; i < C_COMPUTE_UNIT; i++)
            compress_kernel_lz4[i] = new cl::Kernel(*m_program, compress_kernel_names[i].c_str());

        // Create Decompress kernels
        for (uint32_t i = 0; i < D_COMPUTE_UNIT; i++)
            decompress_kernel_lz4[i] = new cl::Kernel(*m_program, decompress_kernel_names[i].c_str());
    } else {
        if (m_bin_flow) {
            // Create Compress kernels
            for (uint32_t i = 0; i < C_COMPUTE_UNIT; i++)
                compress_kernel_lz4[i] = new cl::Kernel(*m_program, compress_kernel_names[i].c_str());
        } else {
            // Create Decompress kernels
            for (uint32_t i = 0; i < D_COMPUTE_UNIT; i++)
                decompress_kernel_lz4[i] = new cl::Kernel(*m_program, decompress_kernel_names[i].c_str());
        }
    }

    return 0;
}

int xfLz4::release() {
    if (m_bin_flow) {
        for (uint32_t i = 0; i < C_COMPUTE_UNIT; i++) delete (compress_kernel_lz4[i]);
    } else {
        for (uint32_t i = 0; i < D_COMPUTE_UNIT; i++) delete (decompress_kernel_lz4[i]);
    }
    delete (m_program);
    delete (m_q);
    delete (m_context);

    return 0;
}

uint64_t xfLz4::decompressFile(std::string& inFile_name, std::string& outFile_name, uint64_t input_size) {
    if (m_switch_flow == 0) {
        std::ifstream inFile(inFile_name.c_str(), std::ifstream::binary);
        std::ofstream outFile(outFile_name.c_str(), std::ofstream::binary);

        if (!inFile) {
            std::cout << "Unable to open file";
            exit(1);
        }

        std::vector<uint8_t, aligned_allocator<uint8_t> > in(input_size);

        // Read magic header 4 bytes
        char c = 0;
        char magic_hdr[] = {MAGIC_BYTE_1, MAGIC_BYTE_2, MAGIC_BYTE_3, MAGIC_BYTE_4};
        for (uint32_t i = 0; i < MAGIC_HEADER_SIZE; i++) {
            inFile.get(c);
            if (c == magic_hdr[i])
                continue;
            else {
                std::cout << "Problem with magic header " << c << " " << i << std::endl;
                exit(1);
            }
        }

        // Header Checksum
        inFile.get(c);

        // Check if block size is 64 KB
        inFile.get(c);
        // printf("block_size %d \n", c);

        switch (c) {
            case BSIZE_STD_64KB:
                m_block_size_in_kb = 64;
                break;
            case BSIZE_STD_256KB:
                m_block_size_in_kb = 256;
                break;
            case BSIZE_STD_1024KB:
                m_block_size_in_kb = 1024;
                break;
            case BSIZE_STD_4096KB:
                m_block_size_in_kb = 4096;
                break;
            default:
                std::cout << "Invalid Block Size" << std::endl;
                break;
        }
        // printf("m_block_size_in_kb %d \n", m_block_size_in_kb);

        // Original size
        uint64_t original_size = 0;
        inFile.read((char*)&original_size, 8);
        inFile.get(c);
        // printf("original_size %d \n", original_size);

        // Allocat output size
        std::vector<uint8_t, aligned_allocator<uint8_t> > out(original_size);

        // Read block data from compressed stream .lz4
        inFile.read((char*)in.data(), (input_size - 15));

        uint32_t host_buffer_size = (m_block_size_in_kb * 1024) * 32;

        if ((m_block_size_in_kb * 1024) > original_size) host_buffer_size = m_block_size_in_kb * 1024;

        uint64_t debytes;
        // Decompression Sequential multiple cus.
        debytes = decompressSequential(in.data(), out.data(), (input_size - 15), original_size, host_buffer_size);
        outFile.write((char*)out.data(), debytes);
        // Close file
        inFile.close();
        outFile.close();
        return debytes;
    } else {
        std::string command = "../../../common/lz4/lz4 --content-size -f -q -d " + inFile_name;
        system(command.c_str());
        return 0;
    }
}

// Note: Various block sizes supported by LZ4 standard are not applicable to
// this function. It just supports Block Size 64KB
uint64_t xfLz4::decompressSequential(
    uint8_t* in, uint8_t* out, uint64_t input_size, uint64_t original_size, uint32_t host_buffer_size) {
    uint32_t max_num_blks = (host_buffer_size) / (m_block_size_in_kb * 1024);

    for (uint32_t i = 0; i < MAX_COMPUTE_UNITS; i++) {
        for (uint32_t j = 0; j < OVERLAP_BUF_COUNT; j++) {
            // Index calculation
            h_buf_in[i][j].resize(host_buffer_size);
            h_buf_out[i][j].resize(host_buffer_size);
            h_blksize[i][j].resize(max_num_blks);
            h_compressSize[i][j].resize(max_num_blks);

            m_compressSize[i][j].reserve(max_num_blks);
            m_blkSize[i][j].reserve(max_num_blks);
        }
    }

    uint32_t block_size_in_bytes = m_block_size_in_kb * 1024;

    // Total number of blocks exists for this file
    uint32_t total_block_cnt = (original_size - 1) / block_size_in_bytes + 1;
    uint32_t block_cntr = 0;
    uint32_t done_block_cntr = 0;

    uint32_t no_compress_case = 0;
    std::chrono::duration<double, std::nano> kernel_time_ns_1(0);
    uint64_t inIdx = 0;
    uint64_t total_decomression_size = 0;

    uint32_t hostChunk_cu[D_COMPUTE_UNIT];
    uint32_t compute_cu;
    uint64_t output_idx = 0;

    for (uint64_t outIdx = 0; outIdx < original_size; outIdx += host_buffer_size * D_COMPUTE_UNIT) {
        compute_cu = 0;
        uint32_t chunk_size = host_buffer_size;

        // Figure out the chunk size for each compute unit
        for (uint32_t bufCalc = 0; bufCalc < D_COMPUTE_UNIT; bufCalc++) {
            hostChunk_cu[bufCalc] = 0;
            if (outIdx + (chunk_size * (bufCalc + 1)) > original_size) {
                hostChunk_cu[bufCalc] = original_size - (outIdx + host_buffer_size * bufCalc);
                compute_cu++;
                break;
            } else {
                hostChunk_cu[bufCalc] = chunk_size;
                compute_cu++;
            }
        }

        uint32_t nblocks[D_COMPUTE_UNIT];
        uint32_t bufblocks[D_COMPUTE_UNIT];
        uint32_t total_size[D_COMPUTE_UNIT];
        uint32_t buf_size[D_COMPUTE_UNIT];
        uint32_t block_size = 0;
        uint32_t compressed_size = 0;

        for (uint32_t cuProc = 0; cuProc < compute_cu; cuProc++) {
            nblocks[cuProc] = 0;
            buf_size[cuProc] = 0;
            bufblocks[cuProc] = 0;
            total_size[cuProc] = 0;
            for (uint32_t cIdx = 0; cIdx < hostChunk_cu[cuProc];
                 cIdx += block_size_in_bytes, nblocks[cuProc]++, total_size[cuProc] += block_size) {
                if (block_cntr == (total_block_cnt - 1)) {
                    block_size = original_size - done_block_cntr * block_size_in_bytes;
                } else {
                    block_size = block_size_in_bytes;
                }

                std::memcpy(&compressed_size, &in[inIdx], 4);
                inIdx += 4;

                uint32_t tmp = compressed_size;
                tmp >>= 24;

                if (tmp == 128) {
                    uint8_t b1 = compressed_size;
                    uint8_t b2 = compressed_size >> 8;
                    uint8_t b3 = compressed_size >> 16;
                    // uint8_t b4 = compressed_size >> 24;

                    if (b3 == 1) {
                        compressed_size = block_size_in_bytes;
                    } else {
                        uint16_t size = 0;
                        size = b2;
                        size <<= 8;
                        uint16_t temp = b1;
                        size |= temp;
                        compressed_size = size;
                    }
                }

                // Fill original block size and compressed size
                m_blkSize[cuProc][0].data()[nblocks[cuProc]] = block_size;
                m_compressSize[cuProc][0].data()[nblocks[cuProc]] = compressed_size;

                // If compressed size is less than original block size
                if (compressed_size < block_size) {
                    h_compressSize[cuProc][0].data()[bufblocks[cuProc]] = compressed_size;
                    h_blksize[cuProc][0].data()[bufblocks[cuProc]] = block_size;
                    std::memcpy(&(h_buf_in[cuProc][0].data()[buf_size[cuProc]]), &in[inIdx], compressed_size);
                    inIdx += compressed_size;
                    buf_size[cuProc] += block_size_in_bytes;
                    bufblocks[cuProc]++;
                } else if (compressed_size == block_size) {
                    no_compress_case++;
                    // No compression block
                    std::memcpy(&(out[outIdx + cuProc * host_buffer_size + cIdx]), &in[inIdx], block_size);
                    inIdx += block_size;
                } else {
                    assert(0);
                }
                block_cntr++;
                done_block_cntr++;
            }
            assert(total_size[cuProc] <= original_size);

            if (nblocks[cuProc] == 1 && compressed_size == block_size) break;
        } // Cu process done

        for (uint32_t bufC = 0; bufC < compute_cu; bufC++) {
            // Device buffer allocation
            buffer_input[bufC][0] =
                new cl::Buffer(*m_context, CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY,
                               buf_size[bufC], h_buf_in[bufC][0].data());

            buffer_output[bufC][0] =
                new cl::Buffer(*m_context, CL_MEM_USE_HOST_PTR | CL_MEM_WRITE_ONLY,
                               buf_size[bufC], h_buf_out[bufC][0].data());

            buffer_block_size[bufC][0] =
                new cl::Buffer(*m_context, CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY,
                               sizeof(uint32_t) * bufblocks[bufC], h_blksize[bufC][0].data());

            buffer_compressed_size[bufC][0] =
                new cl::Buffer(*m_context, CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY,
                               sizeof(uint32_t) * bufblocks[bufC], h_compressSize[bufC][0].data());
        }

        // Set kernel arguments
        for (uint32_t sArg = 0; sArg < compute_cu; sArg++) {
            uint32_t narg = 0;
            decompress_kernel_lz4[sArg]->setArg(narg++, *(buffer_input[sArg][0]));
            decompress_kernel_lz4[sArg]->setArg(narg++, *(buffer_output[sArg][0]));
            decompress_kernel_lz4[sArg]->setArg(narg++, *(buffer_block_size[sArg][0]));
            decompress_kernel_lz4[sArg]->setArg(narg++, *(buffer_compressed_size[sArg][0]));
            decompress_kernel_lz4[sArg]->setArg(narg++, m_block_size_in_kb);
            decompress_kernel_lz4[sArg]->setArg(narg++, bufblocks[sArg]);
        }

        std::vector<cl::Memory> inBufVec;
        for (uint32_t inVec = 0; inVec < compute_cu; inVec++) {
            inBufVec.push_back(*(buffer_input[inVec][0]));
            inBufVec.push_back(*(buffer_block_size[inVec][0]));
            inBufVec.push_back(*(buffer_compressed_size[inVec][0]));
        }

        // Migrate memory - Map host to device buffers
        m_q->enqueueMigrateMemObjects(inBufVec, 0 /* 0 means from host*/);
        m_q->finish();

        auto kernel_start = std::chrono::high_resolution_clock::now();
        // Kernel invocation
        for (uint32_t task = 0; task < compute_cu; task++) {
            m_q->enqueueTask(*decompress_kernel_lz4[task]);
        }
        m_q->finish();

        auto kernel_end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::nano>(kernel_end - kernel_start);
        kernel_time_ns_1 += duration;

        std::vector<cl::Memory> outBufVec;
        for (uint32_t oVec = 0; oVec < compute_cu; oVec++) outBufVec.push_back(*(buffer_output[oVec][0]));

        // Migrate memory - Map device to host buffers
        m_q->enqueueMigrateMemObjects(outBufVec, CL_MIGRATE_MEM_OBJECT_HOST);
        m_q->finish();

        for (uint32_t cuRead = 0; cuRead < compute_cu; cuRead++) {
            uint32_t bufIdx = 0;
            for (uint32_t bIdx = 0, idx = 0; bIdx < nblocks[cuRead]; bIdx++, idx += block_size_in_bytes) {
                uint32_t block_size = m_blkSize[cuRead][0].data()[bIdx];
                uint32_t compressed_size = m_compressSize[cuRead][0].data()[bIdx];
                if (compressed_size < block_size) {
                    std::memcpy(&out[output_idx], &h_buf_out[cuRead][0].data()[bufIdx], block_size);
                    output_idx += block_size;
                    bufIdx += block_size;
                    total_decomression_size += block_size;
                } else if (compressed_size == block_size) {
                    output_idx += block_size;
                }
            }
        } // CU processing ends

        // Delete device buffers
        for (uint32_t dBuf = 0; dBuf < compute_cu; dBuf++) {
            delete (buffer_input[dBuf][0]);
            delete (buffer_output[dBuf][0]);
            delete (buffer_block_size[dBuf][0]);
            delete (buffer_compressed_size[dBuf][0]);
        }
    } // Top - Main loop ends here

    float throughput_in_mbps_1 = (float)total_decomression_size * 1000 / kernel_time_ns_1.count();
    std::cout << std::fixed << std::setprecision(2) << throughput_in_mbps_1;
    return original_size;

} // End of decompress

// Note: Various block sizes supported by LZ4 standard are not applicable to
// this function. It just supports Block Size 64KB
uint64_t xfLz4::compressSequential(uint8_t* in, uint8_t* out, uint64_t input_size, uint32_t host_buffer_size) {
    uint32_t max_num_blks = (host_buffer_size) / (m_block_size_in_kb * 1024);

    for (uint32_t i = 0; i < MAX_COMPUTE_UNITS; i++) {
        for (uint32_t j = 0; j < OVERLAP_BUF_COUNT; j++) {
            // Index calculation
            h_buf_in[i][j].resize(host_buffer_size);
            h_buf_out[i][j].resize(host_buffer_size);
            h_blksize[i][j].resize(max_num_blks);
            h_compressSize[i][j].resize(max_num_blks);

            m_compressSize[i][j].reserve(max_num_blks);
            m_blkSize[i][j].reserve(max_num_blks);
        }
    }

    uint32_t block_size_in_kb = BLOCK_SIZE_IN_KB;
    uint32_t block_size_in_bytes = block_size_in_kb * 1024;

    uint32_t no_compress_case = 0;

    std::chrono::duration<double, std::nano> kernel_time_ns_1(0);

    // Keeps track of output buffer index
    uint64_t outIdx = 0;

    // Given a input file, we process it as multiple chunks
    // Each compute unit is assigned with a chunk of data
    // In this example HOST_BUFFER_SIZE is the chunk size.
    // For example: Input file = 12 MB
    //              HOST_BUFFER_SIZE = 2MB
    // Each compute unit processes 2MB data per kernel invocation
    uint32_t hostChunk_cu[C_COMPUTE_UNIT];

    // This buffer contains total number of BLOCK_SIZE_IN_KB blocks per CU
    // For Example: HOST_BUFFER_SIZE = 2MB/BLOCK_SIZE_IN_KB = 32block (Block
    // size 64 by default)
    uint32_t total_blocks_cu[C_COMPUTE_UNIT];

    // This buffer holds exact size of the chunk in bytes for all the CUs
    uint32_t bufSize_in_bytes_cu[C_COMPUTE_UNIT];

    // Holds value of total compute units to be
    // used per iteration
    uint32_t compute_cu = 0;

    for (uint64_t inIdx = 0; inIdx < input_size; inIdx += host_buffer_size * C_COMPUTE_UNIT) {
        // Needs to reset this variable
        // As this drives compute unit launch per iteration
        compute_cu = 0;

        // Pick buffer size as predefined one
        // If yet to be consumed input is lesser
        // the reset to required size
        uint32_t buf_size = host_buffer_size;

        // This loop traverses through each compute based current inIdx
        // It tries to calculate chunk size and total compute units need to be
        // launched (based on the input_size)
        for (uint32_t bufCalc = 0; bufCalc < C_COMPUTE_UNIT; bufCalc++) {
            hostChunk_cu[bufCalc] = 0;
            // If amount of data to be consumed is less than HOST_BUFFER_SIZE
            // Then choose to send is what is needed instead of full buffer size
            // based on host buffer macro
            if (inIdx + (buf_size * (bufCalc + 1)) > input_size) {
                hostChunk_cu[bufCalc] = input_size - (inIdx + host_buffer_size * bufCalc);
                compute_cu++;
                break;
            } else {
                hostChunk_cu[bufCalc] = buf_size;
                compute_cu++;
            }
        }
        // Figure out total number of blocks need per each chunk
        // Copy input data from in to host buffer based on the inIdx and cu
        for (uint32_t blkCalc = 0; blkCalc < compute_cu; blkCalc++) {
            uint32_t nblocks = (hostChunk_cu[blkCalc] - 1) / block_size_in_bytes + 1;
            total_blocks_cu[blkCalc] = nblocks;
            std::memcpy(h_buf_in[blkCalc][0].data(), &in[inIdx + blkCalc * host_buffer_size], hostChunk_cu[blkCalc]);
        }

        // Fill the host block size buffer with various block sizes per chunk/cu
        for (uint32_t cuBsize = 0; cuBsize < compute_cu; cuBsize++) {
            uint32_t bIdx = 0;
            uint32_t chunkSize_curr_cu = hostChunk_cu[cuBsize];

            for (uint32_t bs = 0; bs < chunkSize_curr_cu; bs += block_size_in_bytes) {
                uint32_t block_size = block_size_in_bytes;
                if (bs + block_size > chunkSize_curr_cu) {
                    block_size = chunkSize_curr_cu - bs;
                }
                h_blksize[cuBsize][0].data()[bIdx++] = block_size;
            }

            // Calculate chunks size in bytes for device buffer creation
            bufSize_in_bytes_cu[cuBsize] = ((hostChunk_cu[cuBsize] - 1) / BLOCK_SIZE_IN_KB + 1) * BLOCK_SIZE_IN_KB;
        }
        for (uint32_t devbuf = 0; devbuf < compute_cu; devbuf++) {
            // Device buffer allocation
            buffer_input[devbuf][0] =
                new cl::Buffer(*m_context, CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY,
                               bufSize_in_bytes_cu[devbuf], h_buf_in[devbuf][0].data());

            buffer_output[devbuf][0] =
                new cl::Buffer(*m_context, CL_MEM_USE_HOST_PTR | CL_MEM_WRITE_ONLY,
                               bufSize_in_bytes_cu[devbuf], h_buf_out[devbuf][0].data());

            buffer_compressed_size[devbuf][0] =
                new cl::Buffer(*m_context, CL_MEM_USE_HOST_PTR | CL_MEM_WRITE_ONLY,
                               sizeof(uint32_t) * total_blocks_cu[devbuf],h_compressSize[devbuf][0].data() );

            buffer_block_size[devbuf][0] =
                new cl::Buffer(*m_context, CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY,
                               sizeof(uint32_t) * total_blocks_cu[devbuf],h_blksize[devbuf][0].data() );
        }

        // Set kernel arguments
        uint32_t narg = 0;
        for (uint32_t argset = 0; argset < compute_cu; argset++) {
            narg = 0;
            compress_kernel_lz4[argset]->setArg(narg++, *(buffer_input[argset][0]));
            compress_kernel_lz4[argset]->setArg(narg++, *(buffer_output[argset][0]));
            compress_kernel_lz4[argset]->setArg(narg++, *(buffer_compressed_size[argset][0]));
            compress_kernel_lz4[argset]->setArg(narg++, *(buffer_block_size[argset][0]));
            compress_kernel_lz4[argset]->setArg(narg++, block_size_in_kb);
            compress_kernel_lz4[argset]->setArg(narg++, hostChunk_cu[argset]);
        }
        std::vector<cl::Memory> inBufVec;

        for (uint32_t inbvec = 0; inbvec < compute_cu; inbvec++) {
            inBufVec.push_back(*(buffer_input[inbvec][0]));
            inBufVec.push_back(*(buffer_block_size[inbvec][0]));
        }

        // Migrate memory - Map host to device buffers
        m_q->enqueueMigrateMemObjects(inBufVec, 0 /* 0 means from host*/);
        m_q->finish();

        // Measure kernel execution time
        auto kernel_start = std::chrono::high_resolution_clock::now();

        // Fire kernel execution
        for (uint32_t ker = 0; ker < compute_cu; ker++) m_q->enqueueTask(*compress_kernel_lz4[ker]);
        // Wait till kernels complete
        m_q->finish();

        auto kernel_end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::nano>(kernel_end - kernel_start);
        kernel_time_ns_1 += duration;

        // Setup output buffer vectors
        std::vector<cl::Memory> outBufVec;
        for (uint32_t outbvec = 0; outbvec < compute_cu; outbvec++) {
            outBufVec.push_back(*(buffer_output[outbvec][0]));
            outBufVec.push_back(*(buffer_compressed_size[outbvec][0]));
        }

        // Migrate memory - Map device to host buffers
        m_q->enqueueMigrateMemObjects(outBufVec, CL_MIGRATE_MEM_OBJECT_HOST);
        m_q->finish();

        for (uint32_t cuCopy = 0; cuCopy < compute_cu; cuCopy++) {
            // Copy data into out buffer
            // Include compress and block size data
            // Copy data block by block within a chunk example 2MB (64block size) - 32 blocks data
            // Do the same for all the compute units
            uint32_t idx = 0;
            for (uint32_t bIdx = 0; bIdx < total_blocks_cu[cuCopy]; bIdx++, idx += block_size_in_bytes) {
                // Default block size in bytes i.e., 64 * 1024
                uint32_t block_size = block_size_in_bytes;
                if (idx + block_size > hostChunk_cu[cuCopy]) {
                    block_size = hostChunk_cu[cuCopy] - idx;
                }
                uint32_t compressed_size = h_compressSize[cuCopy][0].data()[bIdx];
                assert(compressed_size != 0);

                int orig_block_size = hostChunk_cu[cuCopy];
                int perc_cal = orig_block_size * 10;
                perc_cal = perc_cal / block_size;

                if (compressed_size < block_size && perc_cal >= 10) {
                    memcpy(&out[outIdx], &compressed_size, 4);
                    outIdx += 4;
                    std::memcpy(&out[outIdx], &(h_buf_out[cuCopy][0].data()[bIdx * block_size_in_bytes]),
                                compressed_size);
                    outIdx += compressed_size;
                } else {
                    // No Compression, so copy raw data
                    no_compress_case++;
                    if (block_size == 65536) {
                        out[outIdx++] = 0;
                        out[outIdx++] = 0;
                        out[outIdx++] = 1;
                        out[outIdx++] = 128;
                    } else {
                        uint8_t temp = 0;
                        temp = block_size;
                        out[outIdx++] = temp;
                        temp = block_size >> 8;
                        out[outIdx++] = temp;
                        out[outIdx++] = 0;
                        out[outIdx++] = 128;
                    }
                    std::memcpy(&out[outIdx], &in[inIdx + cuCopy * host_buffer_size + idx], block_size);
                    outIdx += block_size;
                }
            } // End of chunk (block by block) copy to output buffer
        }     // End of CU loop - Each CU/chunk block by block copy
        // Buffer deleted
        for (uint32_t bdel = 0; bdel < compute_cu; bdel++) {
            delete (buffer_input[bdel][0]);
            delete (buffer_output[bdel][0]);
            delete (buffer_compressed_size[bdel][0]);
            delete (buffer_block_size[bdel][0]);
        }
    }
    float throughput_in_mbps_1 = (float)input_size * 1000 / kernel_time_ns_1.count();
    std::cout << std::fixed << std::setprecision(2) << throughput_in_mbps_1;
    return outIdx;
}
