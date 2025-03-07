#include "gaussian_extractor.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>
#include <iomanip>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <unistd.h>

// Constants definition
const double R = 8.314462618;  // J/K/mol
const double Po = 101325;      // 1 atm in N/m^2
const double kB = 0.000003166811563;  // Boltzmann constant in Hartree/K

bool compareResults(const Result& a, const Result& b, int column) {
    switch (column) {
        case 2: return a.etgkj < b.etgkj;
        case 3: return a.lf < b.lf;
        case 4: return a.GibbsFreeHartree < b.GibbsFreeHartree;
        case 5: return a.nucleare < b.nucleare;
        case 6: return a.scf < b.scf;
        case 7: return a.zpe < b.zpe;
        default: return false;
    }
}

Result extract(const std::string& file_name_param, double temp, int C, double Po) {
    std::ifstream file(file_name_param);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open file: " + file_name_param);
    }

    std::string file_name = file_name_param;
    if (file_name.substr(0, 2) == "./") {
        file_name = file_name.substr(2);
    }

    std::string line;
    std::vector<std::string> content;
    int copyright_count = 0;
    double scf = 0, zpe = 0, tcg = 0, etg = 0, ezpe = 0, nucleare = 0;
    double scfEqui = 0, scftd = 0;
    std::vector<double> negative_freqs, positive_freqs;
    std::string status = "UNDONE";
    std::string phaseCorr = "NO";

    std::regex scf_pattern(R"(SCF Done.*?=\s+(-?\d+\.\d+))");
    std::regex freq_pattern(R"(Frequencies\s+--\s+(.*))");

    std::vector<double> scf_values;

    while (std::getline(file, line)) {
        content.push_back(line);
        if (content.size() > 10) {
            content.erase(content.begin());
        }

        if (line.find("Copyright") != std::string::npos) {
            ++copyright_count;
        }

        std::smatch match;
        try {
            if (line.find("SCF Done") != std::string::npos && std::regex_search(line, match, scf_pattern)) {
                scf_values.push_back(std::stod(match[1]));
            } else if (line.find("Total Energy, E(CIS") != std::string::npos) {
                scftd = std::stod(line.substr(line.find("=") + 1));
            } else if (line.find("After PCM corrections, the energy is") != std::string::npos) {
                scfEqui = std::stod(line.substr(line.find("is") + 2));
            } else if (line.find("Zero-point correction") != std::string::npos) {
                zpe = std::stod(line.substr(line.find("=") + 1));
            } else if (line.find("Thermal correction to Gibbs Free Energy") != std::string::npos) {
                tcg = std::stod(line.substr(line.find("=") + 1));
            } else if (line.find("Sum of electronic and thermal Free Energies") != std::string::npos) {
                etg = std::stod(line.substr(line.find("=") + 1));
            } else if (line.find("Sum of electronic and zero-point Energies") != std::string::npos) {
                ezpe = std::stod(line.substr(line.find("=") + 1));
            } else if (line.find("nuclear repulsion energy") != std::string::npos) {
                size_t pos = line.find("nuclear repulsion energy") + 25;
                std::string num_str = line.substr(pos);
                num_str.erase(0, num_str.find_first_not_of(" \t"));
                size_t end_pos = num_str.find("Hartrees");
                if (end_pos != std::string::npos) {
                    num_str = num_str.substr(0, end_pos);
                }
                num_str.erase(num_str.find_last_not_of(" \t") + 1);
                if (!num_str.empty()) {
                    nucleare = std::stod(num_str);
                } else {
                    std::cerr << "Warning: Could not parse nuclear repulsion energy from '" << line << "' in file '" << file_name << "'" << std::endl;
                }
            } else if (line.find("Frequencies") != std::string::npos && std::regex_search(line, match, freq_pattern)) {
                std::istringstream iss(match[1]);
                double freq;
                while (iss >> freq) {
                    if (freq < 0) {
                        negative_freqs.push_back(freq);
                    } else {
                        positive_freqs.push_back(freq);
                    }
                }
            } else if (line.find("Kelvin.  Pressure") != std::string::npos) {
                size_t start_pos = line.find("Temperature") + 11;
                size_t end_pos = line.find("Kelvin");
                if (start_pos != std::string::npos && end_pos != std::string::npos && start_pos < end_pos) {
                    std::string temp_str = line.substr(start_pos, end_pos - start_pos);
                    temp_str.erase(0, temp_str.find_first_not_of(" \t"));
                    temp_str.erase(temp_str.find_last_not_of(" \t") + 1);
                    if (!temp_str.empty()) {
                        temp = std::stod(temp_str);
                    } else {
                        std::cerr << "Warning: Could not parse temperature from '" << line << "' in file '" << file_name << "'" << std::endl;
                    }
                }
            } else if (line.find("scrf") != std::string::npos) {
                phaseCorr = "YES";
            }
        } catch (const std::invalid_argument& e) {
            std::cerr << "Error: Invalid number format in file '" << file_name << "' at line: '" << line << "'" << std::endl;
        } catch (const std::out_of_range& e) {
            std::cerr << "Error: Number out of range in file '" << file_name << "' at line: '" << line << "'" << std::endl;
        }
    }

    file.close();

    if (!scf_values.empty()) {
        scf = scf_values.back();
    }

    double lf = !negative_freqs.empty() ? negative_freqs.back() : (!positive_freqs.empty() ? *std::min_element(positive_freqs.begin(), positive_freqs.end()) : 0);

    scf = scfEqui ? scfEqui : (scftd ? scftd : scf);
    etg = etg ? etg : 0.0;
    nucleare = nucleare ? nucleare : 0;
    zpe = zpe ? zpe : 0;

    double GphaseCorr = R * temp * std::log(C * R * temp / Po) * 0.0003808798033989866 / 1000;
    double GibbsFreeHartree = phaseCorr == "YES" && etg != 0.0 ? etg + GphaseCorr : etg;
    double etgkj = GibbsFreeHartree * 2625.5002;

    if (std::any_of(content.begin(), content.end(), [](const std::string& l) { return l.find("Normal termination") != std::string::npos; })) {
        status = "DONE";
    } else if (std::any_of(content.begin(), content.end(), [](const std::string& l) { return l.find("Error termination") != std::string::npos; })) {
        status = "ERROR";
    }

    if (file_name.length() > 53) {
        file_name = file_name.substr(file_name.length() - 53);
    }

    return Result{file_name, etgkj, lf, GibbsFreeHartree, nucleare, scf, zpe, status, phaseCorr, copyright_count};
}

void processAndOutputResults(double temp, int C, int column) {
    auto start_time = std::chrono::high_resolution_clock::now();

    // Get current directory name
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) == nullptr) {
        std::cerr << "Error: Could not get current working directory" << std::endl;
        return;
    }
    std::string dir_name = std::filesystem::path(cwd).filename().string();
    std::string output_filename = dir_name + ".results";
    std::ofstream output_file(output_filename);
    if (!output_file.is_open()) {
        std::cerr << "Could not open output file: " << output_filename << std::endl;
        return;
    }

    std::vector<std::string> log_files;
    for (const auto& entry : std::filesystem::directory_iterator(".")) {
        if (entry.path().extension() == ".log") {
            log_files.push_back(entry.path().string());
        }
    }

    if (log_files.empty()) {
        std::cerr << "No .log files found in the current directory." << std::endl;
        return;
    }

    std::vector<Result> results;
    for (const auto& file : log_files) {
        results.push_back(extract(file, temp, C, Po));
    }

    std::sort(results.begin(), results.end(), [column](const Result& a, const Result& b) {
        return compareResults(a, b, column);
    });

    // Prepare header and separator
    std::ostringstream header;
    header << std::setw(53) << std::left << "Output name"
           << std::setw(18) << std::right << "ETG kJ/mol"
           << std::setw(10) << std::right << "Low FC"
           << std::setw(18) << std::right << "ETG a.u"
           << std::setw(18) << std::right << "Nuclear E au"
           << std::setw(18) << std::right << "SCFE"
           << std::setw(10) << std::right << "ZPE "
           << std::setw(8) << std::right << "Status"
           << std::setw(6) << std::right << "PCorr"
           << std::setw(6) << std::right << "Round" << "\n";

    std::ostringstream separator;
    separator << std::setw(53) << std::left << std::string(53, '-')
              << std::setw(18) << std::right << std::string(18, '-')
              << std::setw(10) << std::right << std::string(10, '-')
              << std::setw(18) << std::right << std::string(18, '-')
              << std::setw(18) << std::right << std::string(18, '-')
              << std::setw(18) << std::right << std::string(18, '-')
              << std::setw(10) << std::right << std::string(10, '-')
              << std::setw(8) << std::right << std::string(8, '-')
              << std::setw(6) << std::right << std::string(6, '-')
              << std::setw(6) << std::right << std::string(6, '-') << "\n";

    // Prepare results output
    std::ostringstream results_stream;
    for (const auto& result : results) {
        results_stream << std::setw(53) << std::left << result.file_name
                       << std::setw(18) << std::right << std::fixed << std::setprecision(6) << result.etgkj
                       << std::setw(10) << std::right << std::fixed << std::setprecision(2) << result.lf
                       << std::setw(18) << std::right << std::fixed << std::setprecision(6) << result.GibbsFreeHartree
                       << std::setw(18) << std::right << std::fixed << std::setprecision(6) << result.nucleare
                       << std::setw(18) << std::right << std::fixed << std::setprecision(6) << result.scf
                       << std::setw(10) << std::right << std::fixed << std::setprecision(6) << result.zpe
                       << std::setw(8) << std::right << result.status
                       << std::setw(6) << std::right << result.phaseCorr
                       << std::setw(6) << std::right << result.copyright_count << "\n";
    }

    // Write to file
    output_file << header.str() << separator.str() << results_stream.str();
    output_file.close();

    // Print to terminal
    std::cout << header.str() << separator.str() << results_stream.str();
    std::cout << "Results written to " << output_filename << std::endl;

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end_time - start_time;
    std::cout << "Total execution time: " << duration.count() << " seconds" << std::endl;
}