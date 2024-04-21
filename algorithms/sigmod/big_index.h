/* A vamana graph index for large groups of points */
#pragma once

#include "virtual_index.h"
#include "small_index.h"

#include "../utils/point_range.h"
#include "../utils/graph.h"
#include "../utils/beamSearch.h"
#include "../vamana/index.h"

#include <algorithm>
#include <iostream>

// limit, degree, alpha
//BuildParams default_build_params = BuildParams(200, 32, 1.175);
BuildParams default_build_params = BuildParams(500, 64, 1.175);

// k, beam size, cut, limit, degree limit
//QueryParams default_query_params = QueryParams(100, 500, 0.9, 1000, 100);
QueryParams default_query_params = QueryParams(100, 500, 1.35, 10000000, 100);

float exhaustive_fallback_cutoff = 0.25;

size_t min_size = 10'000;

void set_default_build_params(long R, long L, double alpha) {
    default_build_params = BuildParams(R, L, alpha);
}

void set_default_query_params(long k, long beamSize, double cut, long limit, long degree_limit) {
    default_query_params = QueryParams(k, beamSize, cut, limit, degree_limit);
}

void set_exhaustive_fallback_cutoff(float cutoff) {
    exhaustive_fallback_cutoff = cutoff;
}

void set_min_size(size_t size) {
    min_size = size;
}

template<typename T, typename Point>
struct VamanaIndex : public VirtualIndex<T, Point> {
    NaiveIndex<T, Point> naive_index;
    Graph<index_type> G;

    std::unique_ptr<VamanaIndex<T, Point>> left = nullptr;
    std::unique_ptr<VamanaIndex<T, Point>> right = nullptr;

    float median_timestamp = -1; // last timestamp in left

    VamanaIndex() = default;

    void fit(PointRange<T, Point>& points,
             parlay::sequence<float>& timestamps,
             parlay::sequence<index_type>& indices) override {
        naive_index.fit(points, timestamps, indices);

        G = Graph<index_type>(default_build_params.R, indices.size());

        knn_index<Point, SubsetPointRange<T, Point, PointRange<T, Point>, uint32_t>, uint32_t> I(default_build_params);

        stats<index_type> BuildStats(points.size());

        I.build_index(G, naive_index.pr, BuildStats);

        if (points.size() >= min_size * 2) {
            parlay::sequence<index_type> left_indices(indices.begin(), indices.begin() + indices.size() / 2);
            parlay::sequence<index_type> right_indices(indices.begin() + indices.size() / 2, indices.end());

            parlay::sequence<float> left_timestamps(timestamps.begin(), timestamps.begin() + timestamps.size() / 2);
            parlay::sequence<float> right_timestamps(timestamps.begin() + timestamps.size() / 2, timestamps.end());

            left = std::make_unique<VamanaIndex<T, Point>>();
            right = std::make_unique<VamanaIndex<T, Point>>();

            NaiveIndex<T, Point> left_ni, right_ni;

            left_ni.copyless_fit(points, left_timestamps, left_indices);
            right_ni.copyless_fit(points, right_timestamps, right_indices);

            left->fit(left_ni);
            right->fit(right_ni);

            median_timestamp = timestamps[indices.size() / 2];
        }
    }

    void fit(PointRange<T, Point>& points,
             parlay::sequence<float>& timestamps) override {
        auto indices = parlay::tabulate(points.size(), [](index_type i) { return i; });

        fit(points, timestamps, indices);
    }

    void fit(NaiveIndex<T, Point>& ni) {
        naive_index = std::move(ni);

        G = Graph<index_type>(default_build_params.R, naive_index.timestamps.size());

        knn_index<Point, SubsetPointRange<T, Point, PointRange<T, Point>, uint32_t>, uint32_t> I(default_build_params);

        stats<index_type> BuildStats(naive_index.timestamps.size());

        I.build_index(G, naive_index.pr, BuildStats);

        
    }

    void knn(Point& query, index_type* out, size_t k) override {
        auto [pairElts, dist_cmps] = beam_search<Point, SubsetPointRange<T, Point, PointRange<T, Point>, uint32_t>, index_type>(query, G, naive_index.pr, 0,default_query_params);

        auto frontier = pairElts.first;

        for (size_t i = 0; i < k; i++) {
            out[i] = naive_index.pr.real_index(frontier[i].first);
        }
    }

    void range_knn(Point& query, index_type* out, std::pair<float, float> endpoints, size_t k) override {
        // we assume that the distribution of timestamps is uniform, and the distance between the endpoints is some good approximation of the selectivity
        float time_range = endpoints.second - endpoints.first;

        if (time_range < exhaustive_fallback_cutoff) {
            naive_index.range_knn(query, out, endpoints, k);
            return;
        } else { // otherwise we use overretrieval
            QueryParams qp = default_query_params;
            qp.k = qp.beamSize;
            qp.limit = static_cast<int>(qp.limit / (endpoints.second - endpoints.first));
            /*qp.k = static_cast<int>((qp.k / (endpoints.second - endpoints.first)) * 1.44);
            qp.beamSize = static_cast<int>((qp.beamSize / (endpoints.second - endpoints.first)) * 1.44);
            qp.limit = static_cast<int>((qp.limit / (endpoints.second - endpoints.first)) * 1.44);*/
 
            auto [pairElts, dist_cmps] = beam_search<Point, SubsetPointRange<T, Point, PointRange<T, Point>, uint32_t>, index_type>(query, G, naive_index.pr, 0, qp);

            auto frontier = pairElts.first;

            // filter to the points which are in the range
            size_t found = 0;
            for (size_t i = 0; i < frontier.size() && found < k; i++) {
                if (naive_index.timestamps[frontier[i].first] >= endpoints.first && naive_index.timestamps[frontier[i].first] <= endpoints.second) {
                    out[found] = naive_index.pr.real_index(frontier[i].first);
                    found++;
                }
            }

            if (found < k) {
                std::cout << "Warning: not enough points in range" << std::endl;
            }
        }
    }
};
