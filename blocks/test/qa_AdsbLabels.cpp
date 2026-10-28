#include <boost/ut.hpp>

#include <complex>
#include <optional>
#include <string_view>

#include <gnuradio-4.0/BlockAttributes.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/meta/reflection.hpp>

#include <adsb/AdsbPrinter.hpp>
#include <adsb/ModeS.hpp>
#include <adsb/PpmFramer.hpp>

/*| role: the test for what the package's three blocks declare to a registry: the key a graph file names each one by,
        the family `adsb` and the role. The registry here is a local one, filled the way a block library's generated
        unit fills the process-wide one, so the attributes read back are the ones a plugin carries.
*/
namespace {

template<typename TBlock>
void expectRegistered(std::string_view key, std::string_view role) {
    using namespace boost::ut;
    expect(eq(gr::meta::type_name<TBlock>(), key)) << "the registry key";

    gr::BlockRegistry registry;
    expect(registry.insert<TBlock>()) << key << "registers";
    const std::optional<gr::property_map> attributes = registry.attributes(key);
    expect(fatal(attributes.has_value())) << key << "carries attributes";

    const gr::block::AttributesRead read = gr::block::attributesFromMap(*attributes, registry.vocabulary());
    expect(read.has(gr::block::LabelClass::Family, "adsb")) << key << "declares the family adsb";
    expect(eq(read.role(), role)) << key << "declares its role";
    expect(eq(read.words(gr::block::LabelClass::Family).size(), 1UZ)) << key << "declares one family";
}

} // namespace

const boost::ut::suite<"AdsbLabels"> adsbLabelTests = [] {
    using namespace boost::ut;
    using namespace std::string_view_literals;

    "the framer over samples"_test    = [] { expectRegistered<adsb::PpmFramer<std::complex<float>>>("adsb::PpmFramer<complex<float32>>"sv, "processor"sv); };
    "the framer over magnitudes"_test = [] { expectRegistered<adsb::PpmFramer<float>>("adsb::PpmFramer<float32>"sv, "processor"sv); };
    "the Mode S decode"_test          = [] { expectRegistered<adsb::ModeSDecode>("adsb::ModeSDecode"sv, "processor"sv); };
    "the printer"_test                = [] { expectRegistered<adsb::AdsbPrinter>("adsb::AdsbPrinter"sv, "sink"sv); };
};

int main() { /* tests are automatically registered and run */ }
