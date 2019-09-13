/*--
 * ---------------------------------------------------------------------------------------------------------------------*/
/*-- DISCLAIMER AND CRITICAL APPLICATIONS */
/*--
 * ---------------------------------------------------------------------------------------------------------------------*/
/*-- */
/*-- (c) Copyright 2019 Xilinx, Inc. All rights reserved. */
/*-- */
/*-- This file contains confidential and proprietary information of Xilinx, Inc.
 * and is protected under U.S. and          */
/*-- international copyright and other intellectual property laws. */
/*-- */
/*-- DISCLAIMER */
/*-- This disclaimer is not a license and does not grant any rights to the
 * materials distributed herewith. Except as      */
/*-- otherwise provided in a valid license issued to you by Xilinx, and to the
 * maximum extent permitted by applicable     */
/*-- law: (1) THESE MATERIALS ARE MADE AVAILABLE "AS IS" AND WITH ALL FAULTS,
 * AND XILINX HEREBY DISCLAIMS ALL WARRANTIES  */
/*-- AND CONDITIONS, EXPRESS, IMPLIED, OR STATUTORY, INCLUDING BUT NOT LIMITED
 * TO WARRANTIES OF MERCHANTABILITY, NON-     */
/*-- INFRINGEMENT, OR FITNESS FOR ANY PARTICULAR PURPOSE; and (2) Xilinx shall
 * not be liable (whether in contract or tort,*/
/*-- including negligence, or under any other theory of liability) for any loss
 * or damage of any kind or nature           */
/*-- related to, arising under or in connection with these materials, including
 * for any direct, or any indirect,          */
/*-- special, incidental, or consequential loss or damage (including loss of
 * data, profits, goodwill, or any type of      */
/*-- loss or damage suffered as a retVal of any action brought by a third party)
 * even if such damage or loss was          */
/*-- reasonably foreseeable or Xilinx had been advised of the possibility of the
 * same.                                    */
/*-- */
/*-- CRITICAL APPLICATIONS */
/*-- Xilinx products are not designed or intended to be fail-safe, or for use in
 * any application requiring fail-safe      */
/*-- performance, such as life-support or safety devices or systems, Class III
 * medical devices, nuclear facilities,       */
/*-- applications related to the deployment of airbags, or any other
 * applications that could lead to death, personal      */
/*-- injury, or severe property or environmental damage (individually and
 * collectively, "Critical                         */
/*-- Applications"). Customer assumes the sole risk and liability of any use of
 * Xilinx products in Critical               */
/*-- Applications, subject only to applicable laws and regulations governing
 * limitations on product liability.            */
/*-- */
/*-- THIS COPYRIGHT NOTICE AND DISCLAIMER MUST BE RETAINED AS PART OF THIS FILE
 * AT ALL TIMES.                             */
/*--
 * ---------------------------------------------------------------------------------------------------------------------*/

#include "xf_fintech_error_codes.hpp"
#include "xf_fintech_trace.hpp"

#include "models/xf_fintech_hcf.hpp"
#include "xf_fintech_hcf_kernel_constants.hpp"

using namespace xf::fintech;

#define XSTR(X) STR(X)
#define STR(X) #X

hcf::hcf() {
    m_pContext = nullptr;
    m_pCommandQueue = nullptr;
    m_pProgram = nullptr;
    m_pHcfKernel = nullptr;

    m_hostInputBuffer.clear();
    m_hostOutputBuffer.clear();

    m_pHwInputBuffer = nullptr;
    m_pHwOutputBuffer = nullptr;

    m_dw = 0.5;
    m_w_max = 200;
}

hcf::~hcf() {
    if (deviceIsPrepared()) {
        releaseDevice();
    }
}

std::string hcf::getXCLBINName(Device* device) {
    std::string xclbinName;
    std::string deviceTypeString;
    std::string dataTypeString;

    deviceTypeString = device->getDeviceTypeString();
    dataTypeString = XSTR(TEST_DT);

    xclbinName = "hcf_hw_" + deviceTypeString + "_" + dataTypeString + ".xclbin";

    return xclbinName;
}

int hcf::createOCLObjects(Device* device) {
    int retval = XLNX_OK;
    cl_int cl_retval = CL_SUCCESS;
    std::string xclbinName;

    std::chrono::time_point<std::chrono::high_resolution_clock> start;
    std::chrono::time_point<std::chrono::high_resolution_clock> end;

    cl::Device clDevice;
    clDevice = device->getCLDevice();
    m_pContext = new cl::Context(clDevice, nullptr, nullptr, nullptr, &cl_retval);

    if (cl_retval == CL_SUCCESS) {
        m_pCommandQueue = new cl::CommandQueue(
            *m_pContext, clDevice, CL_QUEUE_PROFILING_ENABLE | CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE, &cl_retval);
    }

    if (cl_retval == CL_SUCCESS) {
        xclbinName = getXCLBINName(device);

        start = std::chrono::high_resolution_clock::now();
        m_binaries.clear();
        m_binaries = xcl::import_binary_file(xclbinName);
        end = std::chrono::high_resolution_clock::now();

        Trace::printInfo("[XLNX] Binary Import Time = %lld microseconds\n",
                         std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
    }

    /////////////////////////
    // Create PROGRAM Object
    /////////////////////////
    if (cl_retval == CL_SUCCESS) {
        std::vector<cl::Device> devicesToProgram;
        devicesToProgram.push_back(clDevice);

        start = std::chrono::high_resolution_clock::now();
        m_pProgram = new cl::Program(*m_pContext, devicesToProgram, m_binaries, nullptr, &cl_retval);
        end = std::chrono::high_resolution_clock::now();

        Trace::printInfo("[XLNX] Device Programming Time = %lld microseconds\n",
                         std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
    }

    /////////////////////////
    // Create KERNEL Objects
    /////////////////////////
    if (cl_retval == CL_SUCCESS) {
        m_pHcfKernel = new cl::Kernel(*m_pProgram, "hcf_kernel", &cl_retval);
    }

    //////////////////////////
    // Allocate HOST BUFFERS
    //////////////////////////
    m_hostInputBuffer.resize(MAX_OPTION_CALCULATIONS);
    m_hostOutputBuffer.resize(MAX_OPTION_CALCULATIONS);

    ////////////////////////////////
    // Allocate HW BUFFER Objects
    ////////////////////////////////

    size_t sizeInputBufferBytes = sizeof(struct hcfEngineInputDataType<TEST_DT>) * MAX_OPTION_CALCULATIONS;
    size_t sizeOuputBufferBytes = sizeof(TEST_DT) * MAX_OPTION_CALCULATIONS;

    if (cl_retval == CL_SUCCESS) {
        m_pHwInputBuffer = new cl::Buffer(*m_pContext, (CL_MEM_USE_HOST_PTR | CL_MEM_READ_WRITE), sizeInputBufferBytes,
                                          m_hostInputBuffer.data(), &cl_retval);
    }

    if (cl_retval == CL_SUCCESS) {
        m_pHwOutputBuffer = new cl::Buffer(*m_pContext, (CL_MEM_USE_HOST_PTR | CL_MEM_READ_WRITE), sizeOuputBufferBytes,
                                           m_hostOutputBuffer.data(), &cl_retval);
    }

    if (cl_retval != CL_SUCCESS) {
        setCLError(cl_retval);
        Trace::printError("[XLNX] OpenCL Error = %d\n", cl_retval);
        retval = XLNX_ERROR_OPENCL_CALL_ERROR;
    }

    return retval;
}

int hcf::releaseOCLObjects(void) {
    unsigned int i;

    if (m_pHwInputBuffer != nullptr) {
        delete (m_pHwInputBuffer);
        m_pHwInputBuffer = nullptr;
    }

    if (m_pHwOutputBuffer != nullptr) {
        delete (m_pHwOutputBuffer);
        m_pHwOutputBuffer = nullptr;
    }

    if (m_pHcfKernel != nullptr) {
        delete (m_pHcfKernel);
        m_pHcfKernel = nullptr;
    }

    if (m_pProgram != nullptr) {
        delete (m_pProgram);
        m_pProgram = nullptr;
    }

    for (i = 0; i < m_binaries.size(); i++) {
        std::pair<const void*, cl::size_type> binaryPair = m_binaries[i];
        delete[](char*)(binaryPair.first);
    }

    if (m_pCommandQueue != nullptr) {
        delete (m_pCommandQueue);
        m_pCommandQueue = nullptr;
    }

    if (m_pContext != nullptr) {
        delete (m_pContext);
        m_pContext = nullptr;
    }

    return 0;
}

int hcf::run(struct hcf_input_data* inputData, float* outputData, int numOptions) {
    int retval = XLNX_OK;

    if (retval == XLNX_OK) {
        if (deviceIsPrepared()) {
            int num_options = numOptions;

            // prepare the data
            for (int i = 0; i < numOptions; i++) {
                m_hostInputBuffer[i].s0 = inputData[i].s0;
                m_hostInputBuffer[i].v0 = inputData[i].v0;
                m_hostInputBuffer[i].K = inputData[i].K;
                m_hostInputBuffer[i].rho = inputData[i].rho;
                m_hostInputBuffer[i].T = inputData[i].T;
                m_hostInputBuffer[i].r = inputData[i].r;
                m_hostInputBuffer[i].kappa = inputData[i].kappa;
                m_hostInputBuffer[i].vvol = inputData[i].vvol;
                m_hostInputBuffer[i].vbar = inputData[i].vbar;
                m_hostInputBuffer[i].dw = m_dw;
                m_hostInputBuffer[i].w_max = m_w_max;
            }

            // Set the arguments
            m_pHcfKernel->setArg(0, *m_pHwInputBuffer);
            m_pHcfKernel->setArg(1, *m_pHwOutputBuffer);
            m_pHcfKernel->setArg(2, num_options);

            // Copy input data to device global memory
            m_pCommandQueue->enqueueMigrateMemObjects({*m_pHwInputBuffer}, 0);
            m_pCommandQueue->finish();

            // Launch the Kernel
            m_pCommandQueue->enqueueTask(*m_pHcfKernel);
            m_pCommandQueue->finish();

            // Copy Result from Device Global Memory to Host Local Memory
            m_pCommandQueue->enqueueMigrateMemObjects({*m_pHwOutputBuffer}, CL_MIGRATE_MEM_OBJECT_HOST);
            m_pCommandQueue->finish();

            // --------------------------------
            // Give the caller back the results
            // --------------------------------
            for (int i = 0; i < numOptions; i++) {
                outputData[i] = m_hostOutputBuffer[i];
            }
        } else {
            retval = XLNX_ERROR_DEVICE_NOT_OWNED_BY_SPECIFIED_OCL_CONTROLLER;
        }
    }

    return retval;
}

void hcf::set_dw(int dw) {
    m_dw = dw;
}

void hcf::set_w_max(float w_max) {
    m_w_max = w_max;
}

int hcf::get_dw() {
    return m_dw;
}

float hcf::get_w_max() {
    return m_w_max;
}
