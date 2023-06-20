#include <tiledb/tiledb>

#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>

#include "linalg.h"
#include "ivf_index.h"
#include "ivf_query.h"

namespace py = pybind11;
using Ctx = tiledb::Context;

bool global_debug = true;
double global_time_of_interest;

namespace {


template <typename T>
static void declareVector(py::module& mod, std::string const& suffix) {
  using TVector = Vector<T>;
  using PyTVector = py::class_<TVector>;

  PyTVector cls(mod, ("Vector" + suffix).c_str(), py::buffer_protocol());

  cls.def(py::init<T>());
  cls.def("size", &TVector::num_rows);
  cls.def("__getitem__", [](TVector& self, size_t i) { return self[i]; });
  cls.def("__setitem__", [](TVector& self, size_t i) { return self[i]; });
  cls.def_buffer([](TVector &m) -> py::buffer_info {
        return py::buffer_info(
            m.data(),                               /* Pointer to buffer */
            sizeof(T),                          /* Size of one scalar */
            py::format_descriptor<T>::format(), /* Python struct-style format descriptor */
            1,                                      /* Number of dimensions */
            { m.num_rows() },                 /* Buffer dimensions */
            { sizeof(T) }
        );
    });

}

template <typename T>
static void declareColMajorMatrix(py::module& mod, std::string const& suffix) {
  using value_type = T;
  using TMatrix = ColMajorMatrix<T>;
  using PyTMatrix = py::class_<TMatrix>;

  PyTMatrix cls(mod, ("ColMajorMatrix" + suffix).c_str(), py::buffer_protocol());

  cls.def(py::init<size_t, size_t>());
  cls.def("size", &TMatrix::num_rows);
  cls.def_property_readonly("dtype", [](TMatrix& self) -> py::dtype {
      return py::dtype(py::format_descriptor<T>::format());
  });
  cls.def("__getitem__", [](TMatrix& self, std::pair<size_t, size_t> v) {
    // TODO: check bounds
    return self(v.first, v.second); });
  cls.def("__setitem__", [](TMatrix& self, std::pair<size_t, size_t> v, T val) {
    // TODO: check bounds
    self(v.first, v.second) = val;
  });
  cls.def_buffer([](TMatrix &m) -> py::buffer_info {
        return py::buffer_info(
            m.data(),                               /* Pointer to buffer */
            sizeof(T),                          /* Size of one scalar */
            py::format_descriptor<T>::format(), /* Python struct-style format descriptor */
            2,                                      /* Number of dimensions */
            { m.num_rows(), m.num_cols() },                 /* Buffer dimensions */
            { sizeof(T), sizeof(T) * m.num_rows() }
        );
    });

}

template <typename T>
static void declare_kmeans_query(py::module& m, const std::string& suffix) {
  m.def(("kmeans_query_" + suffix).c_str(),
      [](Ctx ctx,
         const std::string& part_uri,
         const ColMajorMatrix<float>& centroids,
         const ColMajorMatrix<float>& query_vectors,
         std::vector<uint64_t>& indices,
         const std::string& id_uri,
         size_t nprobe,
         size_t k_nn,
         bool nth,
         size_t nthreads) -> ColMajorMatrix<size_t> {
        auto r = detail::ivf::qv_query_heap_infinite_ram<T>(
            ctx,
            part_uri,
            centroids,
            query_vectors,
            indices,
            id_uri,
            nprobe,
            k_nn,
            nth,
            nthreads);
        return r;
        }, py::keep_alive<1,2>());
}

template <typename T>
static void declare_pyarray_to_matrix(py::module& m, const std::string& suffix) {
  m.def(("pyarray_copyto_matrix" + suffix).c_str(),
      [](py::array_t<T> arr) -> ColMajorMatrix<T> {
        py::buffer_info info = arr.request();
        if (info.ndim != 2)
          throw std::runtime_error("Number of dimensions must be two");
        if (info.format != py::format_descriptor<T>::format())
          throw std::runtime_error("Mismatched buffer format!");

        auto data = std::unique_ptr<T[]>{new T[info.shape[0] * info.shape[1]]};
        auto r = ColMajorMatrix<T>(std::move(data), info.shape[0], info.shape[1]);
        return r;
        });
}


// Declarations for typed subclasses of ColMajorMatrix
template <typename P>
static void declareColMajorMatrixSubclass(py::module& mod,
    std::string const& name,
    std::string const& suffix) {
  using T = typename P::value_type;
  using TMatrix = P;
  using PyTMatrix = py::class_<TMatrix, ColMajorMatrix<T>>;

  // TODO auto-namify
  PyTMatrix cls(mod, (name + suffix).c_str(), py::buffer_protocol());
  cls.def(py::init<const Ctx&, std::string, size_t>(),  py::keep_alive<1,2>());
}

}

PYBIND11_MODULE(_tiledbvspy, m) {

  py::class_<tiledb::Context> (m, "Ctx", py::module_local())
    .def(py::init([](std::optional<py::dict> maybe_config) {
      tiledb::Config cfg;
      if (maybe_config.has_value()) {
        for (auto item : maybe_config.value()) {
            cfg.set(item.first.cast<std::string>(), item.second.cast<std::string>());
        }
      }
      return tiledb::Context(cfg);
    }
  ));

  /* === Vector === */

  declareVector<uint32_t>(m, "_u32");
  declareVector<uint64_t>(m, "_u64");
  declareVector<float>(m, "_f32");
  declareVector<double>(m, "_f64");

  m.def("read_vector_u32", &read_vector<uint32_t>, "Read a vector from TileDB");
  m.def("read_vector_u64", &read_vector<uint64_t>, "Read a vector from TileDB");


  /* === Matrix === */

  // template specializations
  declareColMajorMatrix<uint8_t>(m, "_u8");
  declareColMajorMatrix<float>(m, "_f32");
  declareColMajorMatrix<double>(m, "_f64");
  declareColMajorMatrix<int32_t>(m, "_i32");
  declareColMajorMatrix<int64_t>(m, "_i64");
  declareColMajorMatrix<uint32_t>(m, "_u32");
  declareColMajorMatrix<uint64_t>(m, "_u64");
  if constexpr (!std::is_same<uint64_t, unsigned long>::value) {
    // Required for a return type, but these types are equivalent on linux :/
    declareColMajorMatrix<unsigned long>(m, "_ul");
  }

  declareColMajorMatrixSubclass<tdbColMajorMatrix<uint8_t>>(
      m, "tdbColMajorMatrix", "_u8");
  declareColMajorMatrixSubclass<tdbColMajorMatrix<uint64_t>>(
      m, "tdbColMajorMatrix", "_u64");
  declareColMajorMatrixSubclass<tdbColMajorMatrix<float>>(
      m, "tdbColMajorMatrix", "_f32");
  declareColMajorMatrixSubclass<tdbColMajorMatrix<int32_t>>(
      m, "tdbColMajorMatrix", "_i32");
  declareColMajorMatrixSubclass<tdbColMajorMatrix<int64_t>>(
      m, "tdbColMajorMatrix", "_i64");

  // Converters from pyarray to matrix
  declare_pyarray_to_matrix<uint8_t>(m, "_u8");
  declare_pyarray_to_matrix<uint64_t>(m, "_u64");
  declare_pyarray_to_matrix<float>(m, "_f32");
  declare_pyarray_to_matrix<double>(m, "_f64");

  /* Query API */

  m.def("query_vq_f32",
        [](const ColMajorMatrix<float>& data,
           const ColMajorMatrix<float>& query_vectors,
           int k,
           bool nth,
           size_t nthreads) -> ColMajorMatrix<uint64_t> {
          auto r = detail::flat::vq_query_heap(data, query_vectors, k, nthreads);
          return r;
        });

  m.def("query_vq_u8",
        [](const ColMajorMatrix<uint8_t>& data,
           const ColMajorMatrix<float>& query_vectors,
           int k,
           bool nth,
           size_t nthreads) -> ColMajorMatrix<uint64_t> {
          auto r = detail::flat::vq_query_heap(data, query_vectors, k, nthreads);
          return r;
        });

  m.def("validate_top_k_u64",
      [](const ColMajorMatrix<uint64_t>& top_k,
         const ColMajorMatrix<int32_t>& ground_truth) -> bool {
        return validate_top_k(top_k, ground_truth);
      });

  declare_kmeans_query<uint8_t>(m, "u8");
  declare_kmeans_query<float>(m, "f32");

}
