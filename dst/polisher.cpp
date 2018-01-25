/*!
 * @file polisher.cpp
 *
 * @brief Polisher class source file
 */

#include <unordered_map>

#include "overlap.hpp"
#include "sequence.hpp"
#include "window.hpp"
#include "polisher.hpp"

#include "bioparser/bioparser.hpp"
#include "thread_pool/thread_pool.hpp"
#include "spoa/spoa.hpp"

namespace racon {

constexpr uint32_t kChunkSize = 1024 * 1024 * 1024; // ~ 1GB

template<class T>
void shrinkToFit(std::vector<std::unique_ptr<T>>& src, uint64_t begin) {

    uint64_t i = begin;
    for (uint64_t j = begin; i < src.size(); ++i) {
        if (src[i] != nullptr) {
            continue;
        }

        j = std::max(j, i);
        while (j < src.size() && src[j] == nullptr) {
            ++j;
        }

        if (j >= src.size()) {
            break;
        } else if (i != j) {
            src[i].swap(src[j]);
        }
    }
    if (i < src.size()) {
        src.resize(i);
    }
}

std::unique_ptr<Polisher> createPolisher(const std::string& sequences_path,
    const std::string& overlaps_path, const std::string& target_path,
    PolisherType type, uint32_t window_length, double quality_threshold,
    double error_threshold, int8_t match, int8_t mismatch, int8_t gap,
    uint32_t num_threads) {

    std::unique_ptr<bioparser::Parser<Sequence>> sparser = nullptr,
        tparser = nullptr;
    std::unique_ptr<bioparser::Parser<Overlap>> oparser = nullptr;

    auto extension = sequences_path.substr(std::min(sequences_path.rfind('.'),
        sequences_path.size()));
    if (extension == ".fasta" || extension == ".fa") {
        sparser = bioparser::createParser<bioparser::FastaParser, Sequence>(
            sequences_path);
    } else if (extension == ".fastq" || extension == ".fq") {
        sparser = bioparser::createParser<bioparser::FastqParser, Sequence>(
            sequences_path);
    } else {
        fprintf(stderr, "racon::createPolisher error: "
            "file %s has unsupported format extension (valid extensions: "
            ".fasta, .fa, .fastq, .fq)!\n", sequences_path.c_str());
        exit(1);
    }

    extension = overlaps_path.substr(std::min(overlaps_path.rfind('.'),
        overlaps_path.size()));
    if (extension == ".mhap") {
        oparser = bioparser::createParser<bioparser::MhapParser, Overlap>(
            overlaps_path);
    } else if (extension == ".paf") {
        oparser = bioparser::createParser<bioparser::PafParser, Overlap>(
            overlaps_path);
    } else if (extension == ".sam") {
        oparser = bioparser::createParser<bioparser::SamParser, Overlap>(
            overlaps_path);
    } else {
        fprintf(stderr, "racon::createPolisher error: "
            "file %s has unsupported format extension (valid extensions: "
            ".mhap, .paf, .sam)!\n", overlaps_path.c_str());
        exit(1);
    }

    extension = target_path.substr(std::min(target_path.rfind('.'),
        target_path.size()));
    if (extension == ".fasta" || extension == ".fa") {
        tparser = bioparser::createParser<bioparser::FastaParser, Sequence>(
            target_path);
    } else if (extension == ".fastq" || extension == ".fq") {
        tparser = bioparser::createParser<bioparser::FastqParser, Sequence>(
            target_path);
    } else {
        fprintf(stderr, "racon::createPolisher error: "
            "file %s has unsupported format extension (valid extensions: "
            ".fasta, .fa, .fastq, .fq)!\n", target_path.c_str());
        exit(1);
    }

    return std::unique_ptr<Polisher>(new Polisher(std::move(sparser),
        std::move(oparser), std::move(tparser), type, window_length,
        quality_threshold, error_threshold, match, mismatch, gap,
        num_threads));
}

Polisher::Polisher(std::unique_ptr<bioparser::Parser<Sequence>> sparser,
    std::unique_ptr<bioparser::Parser<Overlap>> oparser,
    std::unique_ptr<bioparser::Parser<Sequence>> tparser,
    PolisherType type, uint32_t window_length, double quality_threshold,
    double error_threshold, int8_t match, int8_t mismatch, int8_t gap,
    uint32_t num_threads)
        : sparser_(std::move(sparser)), oparser_(std::move(oparser)),
        tparser_(std::move(tparser)), type_(type), quality_threshold_(
        quality_threshold), error_threshold_(error_threshold),
        alignment_engines_(), window_length_(window_length), windows_(),
        thread_pool_(thread_pool::createThreadPool(num_threads)),
        thread_to_id_() {

    uint32_t id = 0;
    for (const auto& it: thread_pool_->thread_identifiers()) {
        thread_to_id_[it] = id++;
    }

    for (uint32_t i = 0; i < num_threads; ++i) {
        alignment_engines_.emplace_back(spoa::createAlignmentEngine(
            spoa::AlignmentType::kNW, match, mismatch, gap));
        alignment_engines_.back()->prealloc(window_length_, 5);
    }
}

Polisher::~Polisher() {
}

void Polisher::initialize() {

    std::vector<std::unique_ptr<Sequence>> sequences;
    std::unordered_map<std::string, uint32_t> name_to_id;
    std::unordered_map<uint32_t, uint32_t> id_to_id;

    tparser_->reset();
    tparser_->parse_objects(sequences, -1);

    fprintf(stderr, "Size = %u\n", sequences.size());

    uint32_t num_targets = sequences.size();
    for (uint32_t i = 0; i < sequences.size(); ++i) {
        fprintf(stderr, "%s\n", sequences[i]->name().c_str());
        name_to_id[sequences[i]->name() + "1"] = i;
        id_to_id[i << 1 | 1] = i;
    }

    uint32_t average_sequence_length = 0;
    uint32_t num_sequences = 0;
    uint32_t new_id = num_targets;

    sparser_->reset();
    while (true) {
        uint32_t l = sequences.size();
        auto status = sparser_->parse_objects(sequences, kChunkSize);

        for (uint32_t i = l; i < sequences.size(); ++i, ++num_sequences) {
            average_sequence_length += sequences[i]->data().size();

            auto it = name_to_id.find(sequences[i]->name() + "1");
            if (it != name_to_id.end()) {
                sequences[i].reset();
                name_to_id[sequences[i]->name() + "0"] = it->second;
                id_to_id[num_sequences << 1 | 0] = it->second;
            } else {
                name_to_id[sequences[i]->name() + "0"] = new_id;
                id_to_id[num_sequences << 1 | 0] = new_id;
                ++new_id;
            }
        }

        shrinkToFit(sequences, l);

        if (!status) {
            break;
        }
    }

    average_sequence_length /= num_sequences;
    WindowType window_type = average_sequence_length <= 1000 ? WindowType::kNGS :
        WindowType::kTGS;

    std::vector<std::future<void>> thread_futures;
    for (uint32_t i = 0; i < num_sequences; ++i) {
        thread_futures.emplace_back(thread_pool_->submit_task(
            [&](uint32_t j) -> void {
                sequences[j]->create_reverse_complement();
            }, id_to_id[i << 1 | 0]));
    }
    for (const auto& it: thread_futures) {
        it.wait();
    }
    thread_futures.clear();

    std::vector<std::unique_ptr<Overlap>> overlaps;

    auto remove_invalid_overlaps = [&](uint64_t begin, uint64_t end) -> void {
        for (uint64_t i = begin; i < end; ++i) {
            if (overlaps[i] == nullptr) {
                continue;
            }
            if (overlaps[i]->error() > error_threshold_) {
                overlaps[i].reset();
                continue;
            }
            if (type_ == PolisherType::kC) {
                for (uint64_t j = i + 1; j < end; ++j) {
                    if (overlaps[j] == nullptr) {
                        continue;
                    }
                    if (overlaps[i]->error() < overlaps[j]->error()) {
                        overlaps[j].reset();
                    } else {
                        overlaps[i].reset();
                        break;
                    }
                }
            }
        }
    };

    oparser_->reset();
    uint64_t l = 0;
    while (true) {
        auto status = oparser_->parse_objects(overlaps, kChunkSize);

        uint64_t c = l;
        for (uint64_t i = l; i < overlaps.size(); ++i) {
            if (!overlaps[i]->is_valid()) {
                overlaps[i].reset();
                continue;
            }
            overlaps[i]->transmute(name_to_id, id_to_id);

            while (overlaps[c] == nullptr) {
                ++c;
            }
            if (overlaps[c]->q_id() != overlaps[i]->q_id()) {
                remove_invalid_overlaps(c, i);
                c = i;
            } else if (!status && i + 1 == overlaps.size()) {
                remove_invalid_overlaps(c, i + 1);
                c = i + 1;
            }
        }

        uint64_t n = 0;
        for (uint64_t i = l; i < c; ++i) {
            if (overlaps[i] != nullptr) {
                thread_futures.emplace_back(thread_pool_->submit_task(
                    [&](uint64_t j) -> void {
                        overlaps[j]->find_breaking_points(sequences, window_length_);
                    }, i));
            } else {
                ++n;
            }
        }
        for (const auto& it: thread_futures) {
            it.wait();
        }
        thread_futures.clear();

        shrinkToFit(overlaps, l);
        l = c - n;

        if (!status) {
            break;
        }
    }

    std::string dummy_backbone_quality(window_length_ * 2, '!');
    std::vector<uint32_t> window_index(num_targets + 1, 0);

    for (uint32_t i = 0; i < num_targets; ++i) {
        uint32_t k = 0;
        for (uint32_t j = 0; j < sequences[i]->data().size(); j += window_length_, ++k) {

            uint32_t length = std::min(j + window_length_,
                static_cast<uint32_t>(sequences[i]->data().size())) - j;

            windows_.emplace_back(createWindow(i, k, window_type,
                &(sequences[i]->data()[j]), length,
                sequences[i]->quality().empty() ? &(dummy_backbone_quality[0]) :
                &(sequences[i]->quality()[j]), length));
        }

        window_index[i + 1] = window_index[i] + k;
    }

    std::string dummy_layer_quality(window_length_ * 2, '"');
    uint32_t low_coverage = 0;
    for (const auto& it: overlaps) {
        const auto& breaking_points = it->breaking_points();

        for (uint32_t i = 1; i < breaking_points.size(); ++i) {
            if (breaking_points[i].second - breaking_points[i - 1].second < 0.02 * window_length_) {
                continue;
            }

            double average_quality = 0;
            for (uint32_t j = breaking_points[i - 1].second; j < breaking_points[i].second; ++j) {
                average_quality += sequences[it->q_id()]->quality()[j] - 33;
            }
            average_quality /= breaking_points[i].second - breaking_points[i - 1].second;

            if (average_quality < quality_threshold_) {
                ++low_coverage;
                continue;
            }

            uint32_t window_id = window_index[it->t_id()] +
                breaking_points[i - 1].first / 500;
            uint32_t window_start = (breaking_points[i - 1].first / 500) * 500;

            const char* sequence = it->strand() ?
                &(sequences[it->q_id()]->reverse_complement()[breaking_points[i - 1].second]) :
                &(sequences[it->q_id()]->data()[breaking_points[i - 1].second]);

            const char* quality = sequences[it->q_id()]->quality().empty() ?
                &(dummy_layer_quality[0]) : (it->strand() ?
                &(sequences[it->q_id()]->reverse_quality()[breaking_points[i - 1].second]) :
                &(sequences[it->q_id()]->quality()[breaking_points[i - 1].second]));

            uint32_t length = breaking_points[i].second -
                breaking_points[i - 1].second;

            windows_[window_id]->add_layer(sequence, length, quality, length,
                breaking_points[i - 1].first - window_start,
                breaking_points[i].first - window_start - 1);
        }
    }
    fprintf(stderr, "Low coverage = %d\n", low_coverage);
}

void Polisher::polish(std::vector<std::unique_ptr<Sequence>>& dst) {

    std::vector<std::future<void>> thread_futures;
    for (uint32_t i = 0; i < windows_.size(); ++i) {
        thread_futures.emplace_back(thread_pool_->submit_task(
            [&](uint32_t j) {
                this->thread_polish(j);
            }, i));
    }
    for (const auto& it: thread_futures) {
        it.wait();
    }

    fprintf(stdout, ">consensus\n");
    for (const auto& it: windows_) {
        fprintf(stdout, "%s", it->consensus().c_str());
    }
    fprintf(stdout, "\n");
}

void Polisher::thread_polish(uint32_t window_id) const {

    auto it = thread_to_id_.find(std::this_thread::get_id());
    if (it == thread_to_id_.end()) {
        fprintf(stderr, "racon::Polisher::polish error: "
            "thread identifier not present!\n");
        exit(1);
    }

    fprintf(stderr, "T,w = %u,%u\n", it->second, window_id);
    windows_[window_id]->generate_consensus(alignment_engines_[it->second]);
}

}