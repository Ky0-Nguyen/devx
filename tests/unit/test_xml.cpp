#include <sstream>
#include <string>

#include "core/util/xml.hpp"
#include "tests/unit/test_framework.hpp"

using namespace mpi;

namespace {

// Walks a document and returns a compact transcript, so a test can assert the
// whole event sequence rather than one field at a time.
std::string transcript(const std::string& doc, xml::Limits limits = xml::Limits()) {
  xml::Parser p(doc.data(), doc.size(), limits);
  std::string out;
  while (p.next()) {
    switch (p.kind()) {
      case xml::NodeKind::kStartElement:
        out += "<" + p.name();
        for (const auto& a : p.attributes()) out += " " + a.name + "=" + a.value;
        out += p.self_closing() ? "/>" : ">";
        break;
      case xml::NodeKind::kEndElement:
        out += "</" + p.name() + ">";
        break;
      case xml::NodeKind::kText:
        out += "'" + p.text() + "'";
        break;
      default:
        break;
    }
  }
  if (p.failed()) out += "!" + p.error();
  return out;
}

bool has(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

MPI_TEST(xml_reads_elements_attributes_and_text, {"D18"}) {
  MPI_CHECK_EQ(transcript("<a x=\"1\" y='2'>hi</a>"),
               std::string("<a x=1 y=2>'hi'</a>"));
  // A self-closing element is reported once, and never as an end element:
  // a caller tracking depth must not see an unbalanced pair.
  MPI_CHECK_EQ(transcript("<a><b/></a>"), std::string("<a><b/></a>"));
  // Whitespace between elements is structure, not content.
  MPI_CHECK_EQ(transcript("<a>\n  <b>x</b>\n</a>"),
               std::string("<a><b>'x'</b></a>"));
}

MPI_TEST(xml_skips_the_declaration_and_comments, {"D18"}) {
  MPI_CHECK_EQ(transcript("<?xml version=\"1.0\"?><!-- note --><a/>"),
               std::string("<a/>"));
}

MPI_TEST(xml_decodes_the_entities_xctrace_actually_emits, {"J02"}) {
  // Real xctrace frame names contain these: C++ signatures with references
  // and templates come through escaped.
  const auto t = transcript(
      "<frame name=\"f(dyld4::RuntimeState&amp;) const::$_0\"/>");
  MPI_CHECK(has(t, "f(dyld4::RuntimeState&) const::$_0"));
  MPI_CHECK_EQ(transcript("<a>&lt;&gt;&quot;&apos;&amp;</a>"),
               std::string("<a>'<>\"'&'</a>"));
  // Numeric references, decimal and hex.
  MPI_CHECK_EQ(transcript("<a>&#65;&#x42;</a>"), std::string("<a>'AB'</a>"));
}

MPI_TEST(xml_refuses_an_unknown_entity_rather_than_passing_it_through,
         {"J02"}) {
  const auto t = transcript("<a>&myentity;</a>");
  MPI_CHECK(has(t, "unknown entity reference"));
}

MPI_TEST(xml_refuses_a_document_type_declaration, {"J03", "J02"}) {
  // The billion-laughs shape. There is no use for a DTD in this format, and
  // partially honouring one is how that attack works, so the document is
  // refused outright.
  const auto t = transcript(
      "<!DOCTYPE lolz [<!ENTITY lol \"lol\">]><a>&lol;</a>");
  MPI_CHECK(has(t, "markup declarations are not processed"));
}

MPI_TEST(xml_reports_malformed_documents_instead_of_guessing, {"J02", "D19"}) {
  MPI_CHECK(has(transcript("<a"), "unterminated start tag"));
  MPI_CHECK(has(transcript("<a x=1></a>"), "attribute value must be quoted"));
  MPI_CHECK(has(transcript("<a x=\"1></a>"), "unterminated attribute value"));
  MPI_CHECK(has(transcript("</a>"), "end tag with no open element"));
  MPI_CHECK(has(transcript("<a></a"), "expected '>' to close an end tag"));
  MPI_CHECK(has(transcript("<!-- unterminated"), "unterminated comment"));
  MPI_CHECK(has(transcript("<1a/>"), "expected an element or attribute name"));
}

MPI_TEST(xml_enforces_its_limits, {"J03", "section-15"}) {
  xml::Limits tight;
  tight.max_depth = 3;
  MPI_CHECK(has(transcript("<a><b><c><d/></c></b></a>", tight),
                "depth limit"));

  xml::Limits small;
  small.max_bytes = 4;
  MPI_CHECK(has(transcript("<abcdefgh/>", small), "over the configured limit"));

  xml::Limits few;
  few.max_attributes = 1;
  MPI_CHECK(has(transcript("<a x=\"1\" y=\"2\"/>", few),
                "more attributes than the configured limit"));

  xml::Limits shorttext;
  shorttext.max_text_length = 4;
  MPI_CHECK(has(transcript("<a>abcdefghij</a>", shorttext),
                "text node exceeds"));
}

MPI_TEST(xml_reads_cdata_as_text, {"D18"}) {
  MPI_CHECK_EQ(transcript("<a><![CDATA[raw <not markup> &amp]]></a>"),
               std::string("<a>'raw <not markup> &amp'</a>"));
}

MPI_TEST(xml_absent_attribute_differs_from_an_empty_one, {"C18"}) {
  const std::string doc = "<frame name=\"f\" addr=\"\"/>";
  xml::Parser p(doc.data(), doc.size());
  MPI_CHECK(p.next());
  MPI_CHECK(p.attribute("addr").has_value());
  MPI_CHECK_EQ(*p.attribute("addr"), std::string(""));
  // Missing is absent, not empty: "no address recorded" and "the address is
  // the empty string" are different facts.
  MPI_CHECK(!p.attribute("load-addr").has_value());
}

MPI_TEST(xml_skip_element_walks_past_a_subtree, {"D18"}) {
  const std::string doc =
      "<root><skipme><deep><deeper/></deep>text</skipme><after/></root>";
  xml::Parser p(doc.data(), doc.size());
  MPI_CHECK(p.next());  // <root>
  MPI_CHECK(p.next());  // <skipme>
  MPI_CHECK_EQ(p.name(), std::string("skipme"));
  MPI_CHECK(p.skip_element());
  MPI_CHECK(p.next());
  MPI_CHECK_EQ(p.name(), std::string("after"));
  MPI_CHECK(p.self_closing());
}
