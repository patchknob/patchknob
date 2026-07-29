#include <cairo.h>
#include <glib.h>
#include <librsvg/rsvg.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr std::array<char, 8> kPackMagic = { 'S', '2', '4', 'S', 'V', 'G', 'P', '1' };
constexpr uint32_t kPackVersion = 1;
constexpr double kRasterScale = 4.0;

struct Entry {
    std::string asset;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
};

template <typename T>
void writeValue(std::ostream& stream, const T& value)
{
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

cairo_status_t writePng(void* closure, const unsigned char* data, unsigned int length)
{
    auto* output = static_cast<std::vector<unsigned char>*>(closure);
    output->insert(output->end(), data, data + length);
    return CAIRO_STATUS_SUCCESS;
}

std::vector<std::string> parsePluginFilter(const std::string& value)
{
    std::vector<std::string> plugins;
    size_t begin = 0;
    while (begin < value.size()) {
        const size_t end = value.find(',', begin);
        const std::string plugin = value.substr(begin, end == std::string::npos ? end : end - begin);
        if (!plugin.empty()) plugins.push_back(plugin);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return plugins;
}

bool shouldInclude(const fs::path& source, const fs::path& sourceRoot,
                   const std::vector<std::string>& pluginFilter)
{
    if (pluginFilter.empty()) return true;
    const fs::path relative = source.lexically_relative(sourceRoot);
    if (relative.empty()) return false;
    const auto component = relative.begin();
    if (component == relative.end()) return false;
    return std::find(pluginFilter.begin(), pluginFilter.end(), component->string()) != pluginFilter.end();
}

fs::path signaturePath(const fs::path& output)
{
    fs::path signature = output;
    signature += ".signature";
    return signature;
}

std::string sourceSignature(const fs::path& sourceRoot, uint32_t maxDimension,
                            const std::vector<std::string>& pluginFilter,
                            std::error_code& error)
{
    std::vector<fs::path> sources;
    for (fs::recursive_directory_iterator iterator(sourceRoot, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (!iterator->is_regular_file(error) || iterator->path().extension() != ".svg" ||
            !shouldInclude(iterator->path(), sourceRoot, pluginFilter)) continue;
        sources.push_back(iterator->path());
    }
    if (error) return {};
    std::sort(sources.begin(), sources.end());

    std::ostringstream signature;
    signature << "max-dimension=" << maxDimension << '\n';
    signature << "raster-scale=" << kRasterScale << '\n';
    signature << "plugins=";
    for (const std::string& plugin : pluginFilter) signature << plugin << ',';
    signature << '\n';
    for (const fs::path& source : sources) {
        const auto timestamp = fs::last_write_time(source, error);
        const uintmax_t size = fs::file_size(source, error);
        if (error) return {};
        signature << source.lexically_relative(sourceRoot).generic_string() << '\t'
                  << size << '\t' << timestamp.time_since_epoch().count() << '\n';
    }
    return signature.str();
}

bool isUpToDate(const fs::path& sourceRoot, const fs::path& output, uint32_t maxDimension,
                const std::vector<std::string>& pluginFilter)
{
    std::error_code error;
    if (!fs::exists(output, error) || error) return false;
    const std::string expected = sourceSignature(sourceRoot, maxDimension, pluginFilter, error);
    if (error || expected.empty()) return false;

    std::ifstream signature(signaturePath(output), std::ios::binary);
    if (!signature) return false;
    const std::string actual((std::istreambuf_iterator<char>(signature)), std::istreambuf_iterator<char>());
    return actual == expected;
}

bool writeSignature(const fs::path& output, const std::string& signature)
{
    const fs::path destination = signaturePath(output);
    fs::path temporary = destination;
    temporary += ".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream.write(signature.data(), static_cast<std::streamsize>(signature.size()));
    stream.close();
    if (!stream) return false;

    std::error_code error;
    fs::remove(destination, error);
    error.clear();
    fs::rename(temporary, destination, error);
    return !error;
}

bool renderSvg(const fs::path& input, uint32_t maxDimension, uint32_t& width, uint32_t& height,
               std::vector<unsigned char>& png)
{
    GError* error = nullptr;
    RsvgHandle* handle = rsvg_handle_new_from_file(input.string().c_str(), &error);
    if (!handle) {
        if (error) g_error_free(error);
        return false;
    }

    RsvgDimensionData dimensions = {};
    rsvg_handle_get_dimensions(handle, &dimensions);
    if (dimensions.width <= 0 || dimensions.height <= 0) {
        g_object_unref(handle);
        return false;
    }

    const double largest = std::max(dimensions.width, dimensions.height);
    const double scale = std::min(kRasterScale, static_cast<double>(maxDimension) / largest);
    width = static_cast<uint32_t>(std::max(1.0, std::ceil(dimensions.width * scale)));
    height = static_cast<uint32_t>(std::max(1.0, std::ceil(dimensions.height * scale)));
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, static_cast<int>(width), static_cast<int>(height));
    cairo_t* context = cairo_create(surface);
    cairo_set_operator(context, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(context, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(context);
    cairo_set_operator(context, CAIRO_OPERATOR_OVER);
    cairo_scale(context, scale, scale);
    const gboolean rendered = rsvg_handle_render_cairo(handle, context);
    cairo_destroy(context);
    g_object_unref(handle);
    if (!rendered || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return false;
    }

    const cairo_status_t status = cairo_surface_write_to_png_stream(surface, writePng, &png);
    cairo_surface_destroy(surface);
    return status == CAIRO_STATUS_SUCCESS && !png.empty();
}

int pack(const fs::path& sourceRoot, const fs::path& output, uint32_t maxDimension,
         const std::vector<std::string>& pluginFilter)
{
    std::error_code error;
    const fs::path absoluteSourceRoot = fs::absolute(sourceRoot, error);
    if (error || !fs::is_directory(absoluteSourceRoot, error)) {
        std::cerr << "Cardinal plugin root is missing: " << sourceRoot << '\n';
        return 2;
    }
    if (isUpToDate(absoluteSourceRoot, output, maxDimension, pluginFilter)) {
        std::cout << "cardinal_svg_packer: " << output << " is current\n";
        return 0;
    }

    std::vector<fs::path> sources;
    for (fs::recursive_directory_iterator iterator(absoluteSourceRoot, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (iterator->is_regular_file(error) && iterator->path().extension() == ".svg" &&
            shouldInclude(iterator->path(), absoluteSourceRoot, pluginFilter))
            sources.push_back(iterator->path());
    }
    if (error) {
        std::cerr << "Failed to enumerate Cardinal SVGs\n";
        return 3;
    }
    std::sort(sources.begin(), sources.end());

    fs::create_directories(output.parent_path(), error);
    if (error) return 4;
    fs::path temporary = output;
    temporary += ".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return 5;

    stream.write(kPackMagic.data(), static_cast<std::streamsize>(kPackMagic.size()));
    writeValue(stream, kPackVersion);
    writeValue(stream, uint32_t{0});
    writeValue(stream, uint64_t{0});

    std::vector<Entry> entries;
    entries.reserve(sources.size());
    int skipped = 0;
    for (size_t index = 0; index < sources.size(); ++index) {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<unsigned char> png;
        if (!renderSvg(sources[index], maxDimension, width, height, png)) {
            ++skipped;
            continue;
        }
        Entry entry;
        entry.asset = sources[index].lexically_relative(absoluteSourceRoot).generic_string();
        if (entry.asset.empty()) {
            std::cerr << "Failed to make asset path relative: " << sources[index] << '\n';
            return 6;
        }
        entry.width = width;
        entry.height = height;
        entry.offset = static_cast<uint64_t>(stream.tellp());
        entry.size = png.size();
        stream.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        if (!stream) return 7;
        entries.push_back(std::move(entry));
        if ((index + 1) % 100 == 0 || index + 1 == sources.size())
            std::cout << "cardinal_svg_packer: " << index + 1 << '/' << sources.size() << '\n';
    }

    const uint64_t indexOffset = static_cast<uint64_t>(stream.tellp());
    for (const Entry& entry : entries) {
        const uint32_t pathSize = static_cast<uint32_t>(entry.asset.size());
        writeValue(stream, pathSize);
        writeValue(stream, entry.width);
        writeValue(stream, entry.height);
        writeValue(stream, entry.offset);
        writeValue(stream, entry.size);
        stream.write(entry.asset.data(), static_cast<std::streamsize>(entry.asset.size()));
    }
    stream.seekp(0);
    stream.write(kPackMagic.data(), static_cast<std::streamsize>(kPackMagic.size()));
    writeValue(stream, kPackVersion);
    writeValue(stream, static_cast<uint32_t>(entries.size()));
    writeValue(stream, indexOffset);
    stream.close();

    fs::remove(output, error);
    error.clear();
    fs::rename(temporary, output, error);
    if (error) return 8;
    const std::string signature = sourceSignature(absoluteSourceRoot, maxDimension, pluginFilter, error);
    if (error || signature.empty() || !writeSignature(output, signature)) return 9;
    std::cout << "cardinal_svg_packer: packed " << entries.size() << " SVGs";
    if (skipped > 0) std::cout << ", skipped " << skipped;
    if (!pluginFilter.empty()) std::cout << " from " << pluginFilter.size() << " plugin(s)";
    std::cout << " into " << output << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3 || argc > 5) {
        std::cerr << "Usage: cardinal_svg_packer <plugins-root> <output-pack> [max-dimension] [plugin[,plugin...]]\n";
        return 1;
    }
    uint32_t maxDimension = 4096;
    if (argc == 4) maxDimension = static_cast<uint32_t>(std::clamp(std::stoi(argv[3]), 64, 4096));
    if (argc == 5) maxDimension = static_cast<uint32_t>(std::clamp(std::stoi(argv[3]), 64, 4096));
    const std::vector<std::string> pluginFilter = argc == 5 ? parsePluginFilter(argv[4]) : std::vector<std::string>{};
    return pack(argv[1], argv[2], maxDimension, pluginFilter);
}
