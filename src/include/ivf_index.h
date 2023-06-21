/**
 * @file   ivf_index.h
 *
 * @section LICENSE
 *
 * The MIT License
 *
 * @copyright Copyright (c) 2023 TileDB, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * @section DESCRIPTION
 *
 * Header-only library of functions for building an inverted file (IVF) index,
 * generated by kmeans algorithm.
 *
 * The basic use case is:
 * - Create an instance of the index
 * - Call train() to build the index
 * - OR Call load() to load the index from TileDB arrays
 * - Call add() to add vectors to the index (alt. add with ids)
 * - Call search() to query the index, returning the ids of the nearest vectors,
 *   and optionally the distances.
 * - Compute the recall of the search results.
 *
 * - Call save() to save the index to disk
 * - Call reset() to clear the index
 *
 * Still WIP.
 */

#ifndef TILEDB_IVF_INDEX_H
#define TILEDB_IVF_INDEX_H

#include <atomic>
#include <random>
#include <thread>

#include "algorithm.h"
#include "array_types.h"
#include "defs.h"
#include "linalg.h"

#include "detail/flat/qv.h"

template <class T = shuffled_db_type>
class kmeans_index {
  // Random device to seed the random number generator
  std::random_device rd;
  std::mt19937 gen{rd()};

  size_t dimension_{0};
  size_t nlist_;
  size_t max_iter_;
  double tol_;
  size_t nthreads_{std::thread::hardware_concurrency()};

  ColMajorMatrix<T> centroids_;
  std::vector<indices_type> indices_;
  std::vector<indices_type> shuffled_ids_;
  ColMajorMatrix<T> shuffled_db_;

 public:
  kmeans_index(
      size_t dimension,
      size_t nlist,
      size_t max_iter,
      double tol,
      size_t nthreads)
      : dimension_(dimension)
      , nlist_(nlist)
      , max_iter_(max_iter)
      , tol_(tol)
      , nthreads_(nthreads)
      , centroids_(dimension, nlist) {
  }

  /**
   * @brief Use kmeans++ algorithm to choose initial centroids.
   */
  void kmeans_pp(const ColMajorMatrix<T>& training_set) {
    scoped_timer _{__FUNCTION__};

    std::uniform_int_distribution<> dis(0, training_set.num_cols() - 1);
    auto choice = dis(gen);

    std::copy(
        begin(training_set[choice]),
        end(training_set[choice]),
        begin(centroids_[0]));

    //        Choose one center uniformly at random among the data points.
    //        For each data point x not chosen yet, compute D(x), the distance
    //        between x and the nearest center that has already been chosen.
    //            Choose one new data point at random as a new center, using a
    //            weighted probability distribution where a point x is chosen
    //            with probability proportional to D(x)2.
    //        Repeat Steps 2 and 3 until k centers have been chosen.
    //        Now that the initial centers have been chosen, proceed using
    //        standard k-means clustering.

    std::vector<double> distances(
        training_set.num_cols(), std::numeric_limits<double>::max() / 8);

#ifdef _TRIANGLE_INEQUALITY
    std::vector<double> centroid_centroid(nlist_, 0.0);
    std::vector<size_t> nearest_centroid(training_set.num_cols(), 0);
#endif

    // Calculate the remaining centroids using K-means++ algorithm
    for (size_t i = 1; i < nlist_; ++i) {
      std::vector<double> totalDistance(nthreads_, 0.0);
      stdx::execution::indexed_parallel_policy par{nthreads_};

      stdx::range_for_each(
          std::move(par),
          training_set,
          [this, &distances, &totalDistance, i](
              auto&& vec, size_t n, size_t j) {

      // centroid i-1 is the newest centroid

#ifdef _TRIANGLE_INEQUALITY
            // using triangle inequality, only need to calculate distance to the
            // newest centroid if distance between vec and its current nearest
            // centroid is greater than half the distance between the newest
            // centroid and vectors nearest centroid (1/4 distance squared)

            double min_distance = distances[j];
            if (centroid_centroid[nearest_centroid[j]] < 4 * min_distance) {
              double distance = sum_of_squares(vec, centroids_[i - 1]);
              if (distance < min_distance) {
                min_distance = distance;
                nearest_centroid[j] = i - 1;
                distances[j] = min_distance;
              }
            }
#else
            double distance = sum_of_squares(vec, centroids_[i - 1]);
            auto min_distance = std::min(distances[j], distance);
            distances[j] = min_distance;
            totalDistance[n] += min_distance;
#endif
          });
      double total =
          std::accumulate(begin(totalDistance), end(totalDistance), 0.0);

      // This isn't really necessary for the discrete_distribution
      // std::for_each(begin(distances), end(distances), [total](auto& element)
      // {
      //   element /= total;  // Normalize
      // });

      // Select the next centroid based on the probability proportional to
      // distance squared
      std::discrete_distribution<size_t> probabilityDistribution(
          distances.begin(), distances.end());
      size_t nextIndex = probabilityDistribution(gen);
      std::copy(
          begin(training_set[nextIndex]),
          end(training_set[nextIndex]),
          begin(centroids_[i]));
      distances[nextIndex] = 0.0;

#ifdef _TRIANGLE_INEQUALITY
      // Update centroid-centroid distances -- only need distances from each
      // existing to the new one
      centroid_centroid[i] = sum_of_squares(centroids_[i], centroids_[i - 1]);
      for (size_t j = 0; j < i; ++j) {
        centroid_centroid[j] = sum_of_squares(centroids_[i], centroids_[j]);
      }
#endif
    }
  }

  /**
   * @brief Initialize centroids by choosing them at random from training set.
   */
  void kmeans_random_init(const ColMajorMatrix<T>& training_set) {
    scoped_timer _{__FUNCTION__};

    std::vector<size_t> indices(nlist_);
    std::uniform_int_distribution<> dis(0, training_set.num_cols() - 1);
    for (size_t i = 0; i < nlist_; ++i) {
      indices[i] = dis(gen);
    }
    // std::iota(begin(indices), end(indices), 0);
    // std::shuffle(begin(indices), end(indices), gen);

    for (size_t i = 0; i < nlist_; ++i) {
      std::copy(
          begin(training_set[indices[i]]),
          end(training_set[indices[i]]),
          begin(centroids_[i]));
    }
  }

  /**
   * @brief Use kmeans algorithm to cluster vectors into centroids.
   */
  void train_no_init(const ColMajorMatrix<T>& training_set) {
    scoped_timer _{__FUNCTION__};

    std::vector<size_t> degrees(nlist_, 0);

    for (size_t iter = 0; iter < max_iter_; ++iter) {
      auto parts =
          detail::flat::qv_partition(centroids_, training_set, nthreads_);

      // for (auto & p : parts) {
      //   std::cout << p << " ";
      // }
      // std::cout << std::endl;

      for (size_t j = 0; j < nlist_; ++j) {
        std::fill(begin(centroids_[j]), end(centroids_[j]), 0.0);
      }
      std::fill(begin(degrees), end(degrees), 0);

      stdx::execution::indexed_parallel_policy par{nthreads_};

      // @todo parallelize -- use a temp centroid matrix for each thread
      for (size_t i = 0; i < training_set.num_cols(); ++i) {
        auto part = parts[i];
        auto centroid = centroids_[part];
        auto vector = training_set[i];
        for (size_t j = 0; j < dimension_; ++j) {
          centroid[j] += vector[j];
        }
        ++degrees[part];
      }

      auto mm = std::minmax_element(begin(degrees), end(degrees));
      double sum = std::accumulate(begin(degrees), end(degrees), 0);
      double average = sum / (double)size(degrees);

      auto min = *mm.first;
      auto max = *mm.second;
      auto diff = max - min;
      std::cout << "avg: " << average << " sum: " << sum << " min: " << min
                << " max: " << max << " diff: " << diff << std::endl;

      // @todo parallelize


      for (size_t j = 0; j < nlist_; ++j) {
        auto centroid = centroids_[j];
        for (size_t k = 0; k < dimension_; ++k) {
          if (degrees[j] != 0) {
            centroid[k] /= degrees[j];
          }
        }
      }
    }

    // Debugging
#ifdef _SAVE_PARTITIONS
      {
        char tempFileName[L_tmpnam];
        tmpnam(tempFileName);

        std::ofstream file(tempFileName);
        if (!file) {
          std::cout << "Error opening the file." << std::endl;
          return;
        }

        for (const auto& element : degrees) {
          file << element << ',';
        }
        file << std::endl;

        file.close();

        std::cout << "Data written to file: " << tempFileName << std::endl;
      }
#endif
    }

    void train(const ColMajorMatrix<T>& training_set) {
      kmeans_pp(training_set);
      train_no_init(training_set);
    }

#if 0
  // @todo WIP
  void add(const ColMajorMatrix<T>& db) {
    auto parts = detail::flat::qv_partition(centroids_, db, nthreads_);
    std::vector<size_t> degrees(centroids_.num_cols());
    std::vector<indices_type> indices(centroids_.num_cols() + 1);
    std::vector shuffled_ids = std::vector<shuffled_ids_type>(db.num_cols());
    auto shuffled_db = ColMajorMatrix<T>{db.num_rows(), db.num_cols()};

    for (size_t i = 0; i < db.num_cols(); ++i) {
      auto j = parts[i];
      ++degrees[j];
    }
    indices[0] = 0;
    std::inclusive_scan(begin(degrees), end(degrees), begin(indices) + 1);

    std::iota(begin(shuffled_ids), end(shuffled_ids), 0);

    for (size_t i = 0; i < db.num_cols(); ++i) {
      size_t bin = parts[i];
      size_t ibin = indices[bin];

      shuffled_ids[ibin] = i;

      assert(ibin < shuffled_db.num_cols());
      for (size_t j = 0; j < db.num_rows(); ++j) {
        shuffled_db(j, ibin) = db(j, i);
      }
      ++indices[bin];
    }

    std::shift_right(begin(indices), end(indices), 1);
    indices[0] = 0;

    indices_ = std::move(indices);
    shuffled_ids_ = std::move(shuffled_ids);
    shuffled_db_ = std::move(shuffled_db);
  }
#endif

    auto& get_centroids() {
      return centroids_;
    }
  };

#endif  // TILEDB_IVF_INDEX_H