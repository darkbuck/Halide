namespace gemmlowp {


void benchmark(GemmContext* context) {
  std::map<gemm_t, std::vector<double>> benchmark_results;

  std::vector<gemm_t> benchmark_gemms;
  benchmark_gemms.emplace_back(10, 10, 10);
  benchmark_gemms.emplace_back(20, 20, 20);
  benchmark_gemms.emplace_back(30, 30, 30);
  benchmark_gemms.emplace_back(40, 40, 40);
  benchmark_gemms.emplace_back(50, 50, 50);
  benchmark_gemms.emplace_back(60, 60, 60);
  benchmark_gemms.emplace_back(64, 256, 147);
  benchmark_gemms.emplace_back(100, 100, 1);
  benchmark_gemms.emplace_back(100, 100, 100);
  benchmark_gemms.emplace_back(100, 1000, 100);
  benchmark_gemms.emplace_back(1000, 1000, 1);
  benchmark_gemms.emplace_back(1000, 1000, 10);
  benchmark_gemms.emplace_back(1000, 1000, 100);
  benchmark_gemms.emplace_back(1000, 1000, 1000);
}

Gemm<std::uint8_t, GEMMLOWP_TEST_BIT_DEPTH_PARAMS>(
            context, lhs[k].const_map(), rhs[k].const_map(), &result[k].map(),
            -75, -91, 74980, 123, 20);

void benchmark_googlenet(GemmContext* context) {
  // These are the m, n, k sizes for a typical GoogLeNet.
  const int googlenet_gemm_sizes[] = {
      12544, 64,  147, 3136, 64,   64,   3136, 192,  576,  784, 64,   192,
      784,   96,  192, 784,  128,  864,  784,  16,   192,  784, 32,   400,
      784,   32,  192, 784,  128,  256,  784,  128,  256,  784, 192,  1152,
      784,   32,  256, 784,  96,   800,  784,  64,   256,  196, 192,  480,
      196,   96,  480, 196,  204,  864,  196,  16,   480,  196, 48,   400,
      196,   64,  480, 196,  160,  508,  196,  112,  508,  196, 224,  1008,
      196,   24,  508, 196,  64,   600,  196,  64,   508,  196, 128,  512,
      196,   128, 512, 196,  256,  1152, 196,  24,   512,  196, 64,   600,
      196,   64,  512, 196,  112,  512,  196,  144,  512,  196, 288,  1296,
      196,   32,  512, 196,  64,   800,  196,  64,   512,  196, 256,  528,
      196,   160, 528, 196,  320,  1440, 196,  32,   528,  196, 128,  800,
      196,   128, 528, 49,   256,  832,  49,   160,  832,  49,  320,  1440,
      49,    48,  832, 49,   128,  1200, 49,   128,  832,  49,  384,  832,
      49,    192, 832, 49,   384,  1728, 49,   48,   832,  49,  128,  1200,
      49,    128, 832, 16,   128,  508,  1,    1024, 2048, 1,   1008, 1024,
      16,    128, 528, 1,    1024, 2048, 1,    1008, 1024, 1,   1008, 1024,
  };
  assert(sizeof(googlenet_gemm_sizes) % (3 * sizeof(googlenet_gemm_sizes[0])) ==
         0);
  const std::size_t num_googlenet_gemms =
      sizeof(googlenet_gemm_sizes) / (3 * sizeof(googlenet_gemm_sizes[0]));

  std::vector<gemm_t> googlenet_gemms(num_googlenet_gemms);
  for (std::size_t i = 0; i < num_googlenet_gemms; i++) {
    googlenet_gemms[i].rows = googlenet_gemm_sizes[3 * i + 1];
    googlenet_gemms[i].depth = googlenet_gemm_sizes[3 * i + 2];
    googlenet_gemms[i].cols = googlenet_gemm_sizes[3 * i + 0];
  }

  const double mintime = 20.0;
  benchmark_gemm_sizes(context, googlenet_gemms, mintime);
}

void benchmark_small_model(GemmContext* context) {
  // These are the m, n, k sizes for a small model with large batches.
  const int small_model_gemm_sizes[] = {
      29232, 16, 25, 7308, 6, 400, 203, 3002, 216,
  };
  assert(sizeof(small_model_gemm_sizes) %
             (3 * sizeof(small_model_gemm_sizes[0])) ==
         0);
  const std::size_t num_small_model_gemms =
      sizeof(small_model_gemm_sizes) / (3 * sizeof(small_model_gemm_sizes[0]));

  std::vector<gemm_t> small_model_gemms(num_small_model_gemms);
  for (std::size_t i = 0; i < num_small_model_gemms; i++) {
    small_model_gemms[i].rows = small_model_gemm_sizes[3 * i + 1];
    small_model_gemms[i].depth = small_model_gemm_sizes[3 * i + 2];
    small_model_gemms[i].cols = small_model_gemm_sizes[3 * i + 0];
  }

  const double mintime = 10.0;
  benchmark_gemm_sizes(context, small_model_gemms, mintime);
}

void benchmark_all() {
  {
    gemmlowp::GemmContext context;
    std::cout << "Benchmarking small model GEMMs..." << std::endl;
    gemmlowp::benchmark_small_model(&context);
  }

  {
    gemmlowp::GemmContext context;
    std::cout << "Benchmarking typical GoogLeNet GEMMs..." << std::endl;
    gemmlowp::benchmark_googlenet(&context);
  }

  {
    gemmlowp::GemmContext context;
    std::cout << "Benchmarking default mode (typically multi-threaded)..."
              << std::endl;
    gemmlowp::benchmark(&context);
  }

  {
    gemmlowp::GemmContext context;
    context.set_max_num_threads(1);
    std::cout << "Benchmarking single-threaded mode..." << std::endl;
    gemmlowp::benchmark(&context);
  }
}

}  // end namespace gemmlowp

// For iOS, we need to define our own main(), so skip it here.
#if !(defined(__APPLE__) && (TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR))
int main() { gemmlowp::benchmark_all(); }
#endif
