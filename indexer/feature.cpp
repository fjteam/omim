#include "indexer/feature.hpp"

#include "indexer/classificator.hpp"
#include "indexer/feature_algo.hpp"
#include "indexer/feature_loader_base.hpp"
#include "indexer/feature_visibility.hpp"

#include "geometry/distance.hpp"
#include "geometry/robust_orientation.hpp"

#include "platform/preferred_languages.hpp"

#include "defines.hpp" // just for file extensions


using namespace feature;

///////////////////////////////////////////////////////////////////////////////////////////////////
// FeatureBase implementation
///////////////////////////////////////////////////////////////////////////////////////////////////

void FeatureBase::Deserialize(feature::LoaderBase * pLoader, TBuffer buffer)
{
  m_pLoader = pLoader;
  m_pLoader->Init(buffer);

  m_limitRect = m2::RectD::GetEmptyRect();
  m_bTypesParsed = m_bCommonParsed = false;
  m_header = m_pLoader->GetHeader();
}

void FeatureBase::ParseTypes() const
{
  if (!m_bTypesParsed)
  {
    m_pLoader->ParseTypes();
    m_bTypesParsed = true;
  }
}

void FeatureBase::ParseCommon() const
{
  if (!m_bCommonParsed)
  {
    ParseTypes();

    m_pLoader->ParseCommon();
    m_bCommonParsed = true;
  }
}

feature::EGeomType FeatureBase::GetFeatureType() const
{
  switch (Header() & HEADER_GEOTYPE_MASK)
  {
  case HEADER_GEOM_LINE: return GEOM_LINE;
  case HEADER_GEOM_AREA: return GEOM_AREA;
  default: return GEOM_POINT;
  }
}

string FeatureBase::DebugString() const
{
  ParseCommon();

  Classificator const & c = classif();

  string res = "Types";
  for (size_t i = 0; i < GetTypesCount(); ++i)
    res += (" : " + c.GetReadableObjectName(m_types[i]));
  res += "\n";

  return (res + m_params.DebugString());
}


///////////////////////////////////////////////////////////////////////////////////////////////////
// FeatureType implementation
///////////////////////////////////////////////////////////////////////////////////////////////////

void FeatureType::Deserialize(feature::LoaderBase * pLoader, TBuffer buffer)
{
  base_type::Deserialize(pLoader, buffer);

  m_pLoader->InitFeature(this);

  m_bHeader2Parsed = m_bPointsParsed = m_bTrianglesParsed = m_bMetadataParsed = false;

  m_innerStats.MakeZero();
}

void FeatureType::ParseHeader2() const
{
  if (!m_bHeader2Parsed)
  {
    ParseCommon();

    m_pLoader->ParseHeader2();
    m_bHeader2Parsed = true;
  }
}

void FeatureType::ResetGeometry() const
{
  m_points.clear();
  m_triangles.clear();

  if (GetFeatureType() != GEOM_POINT)
    m_limitRect = m2::RectD();

  m_bHeader2Parsed = m_bPointsParsed = m_bTrianglesParsed = false;

  m_pLoader->ResetGeometry();
}

uint32_t FeatureType::ParseGeometry(int scale) const
{
  uint32_t sz = 0;
  if (!m_bPointsParsed)
  {
    ParseHeader2();

    sz = m_pLoader->ParseGeometry(scale);
    m_bPointsParsed = true;
  }
  return sz;
}

uint32_t FeatureType::ParseTriangles(int scale) const
{
  uint32_t sz = 0;
  if (!m_bTrianglesParsed)
  {
    ParseHeader2();

    sz = m_pLoader->ParseTriangles(scale);
    m_bTrianglesParsed = true;
  }
  return sz;
}

void FeatureType::ParseMetadata() const
{
  if (m_bMetadataParsed) return;

  m_pLoader->ParseMetadata();

  if (HasInternet())
    m_metadata.Set(Metadata::FMD_INTERNET, "wlan");

  m_bMetadataParsed = true;
}

void FeatureType::SetNames(StringUtf8Multilang const & newNames)
{
  m_params.name.Clear();
  // Validate passed string to clean up empty names (if any).
  newNames.ForEachRef([this](int8_t langCode, string const & name) -> bool
  {
    if (!name.empty())
      m_params.name.AddString(langCode, name);
    return true;
  });

  if (m_params.name.IsEmpty())
    SetHeader(~feature::HEADER_HAS_NAME & Header());
  else
    SetHeader(feature::HEADER_HAS_NAME | Header());
}

void FeatureType::SetMetadata(feature::Metadata const & newMetadata)
{
  m_bMetadataParsed = true;
  m_metadata = newMetadata;
}

namespace
{
  template <class TCont>
  void Points2String(string & s, TCont const & points)
  {
    for (size_t i = 0; i < points.size(); ++i)
      s += DebugPrint(points[i]) + " ";
  }
}

string FeatureType::DebugString(int scale) const
{
  return base_type::DebugString() + "; Center = " +
         DebugPrint(MercatorBounds::ToLatLon(feature::GetCenter(*this, scale)));
}

string DebugPrint(FeatureType const & ft)
{
  return ft.DebugString(FeatureType::BEST_GEOMETRY);
}

bool FeatureType::IsEmptyGeometry(int scale) const
{
  ParseAll(scale);

  switch (GetFeatureType())
  {
  case GEOM_AREA: return m_triangles.empty();
  case GEOM_LINE: return m_points.empty();
  default: return false;
  }
}

m2::RectD FeatureType::GetLimitRect(int scale) const
{
  ParseAll(scale);

  if (m_triangles.empty() && m_points.empty() && (GetFeatureType() != GEOM_POINT))
  {
    // This function is called during indexing, when we need
    // to check visibility according to feature sizes.
    // So, if no geometry for this scale, assume that rect has zero dimensions.
    m_limitRect = m2::RectD(0, 0, 0, 0);
  }

  return m_limitRect;
}

void FeatureType::ParseAll(int scale) const
{
  ParseGeometry(scale);
  ParseTriangles(scale);
}

FeatureType::geom_stat_t FeatureType::GetGeometrySize(int scale) const
{
  uint32_t sz = ParseGeometry(scale);
  if (sz == 0 && !m_points.empty())
    sz = m_innerStats.m_points;

  return geom_stat_t(sz, m_points.size());
}

FeatureType::geom_stat_t FeatureType::GetTrianglesSize(int scale) const
{
  uint32_t sz = ParseTriangles(scale);
  if (sz == 0 && !m_triangles.empty())
    sz = m_innerStats.m_strips;

  return geom_stat_t(sz, m_triangles.size());
}

struct BestMatchedLangNames
{
  string m_defaultName;
  string m_nativeName;
  string m_intName;
  string m_englishName;

  bool operator()(int8_t code, string const & name)
  {
    static int8_t const defaultCode = StringUtf8Multilang::GetLangIndex("default");
    static int8_t const nativeCode = StringUtf8Multilang::GetLangIndex(languages::GetCurrentNorm());
    static int8_t const intCode = StringUtf8Multilang::GetLangIndex("int_name");
    static int8_t const englishCode = StringUtf8Multilang::GetLangIndex("en");

    if (code == defaultCode)
      m_defaultName = name;
    else if (code == nativeCode)
      m_nativeName = name;
    else if (code == intCode)
    {
      // There are many "junk" names in Arabian island.
      m_intName = name.substr(0, name.find_first_of(','));
      // int_name should be used as name:en when name:en not found
      if ((nativeCode == englishCode) && m_nativeName.empty())
        m_nativeName = m_intName;
    }
    else if (code == englishCode)
      m_englishName = name;
    return true;
  }
};

void FeatureType::GetPreferredNames(string & defaultName, string & intName) const
{
  ParseCommon();

  BestMatchedLangNames matcher;
  ForEachNameRef(matcher);

  defaultName.swap(matcher.m_defaultName);

  if (!matcher.m_nativeName.empty())
    intName.swap(matcher.m_nativeName);
  else if (!matcher.m_intName.empty())
    intName.swap(matcher.m_intName);
  else
    intName.swap(matcher.m_englishName);

  if (defaultName.empty())
    defaultName.swap(intName);
  else
  {
    // filter out similar intName
    if (!intName.empty() && defaultName.find(intName) != string::npos)
      intName.clear();
  }
}

void FeatureType::GetReadableName(string & name) const
{
  ParseCommon();

  BestMatchedLangNames matcher;
  ForEachNameRef(matcher);

  if (!matcher.m_nativeName.empty())
    name.swap(matcher.m_nativeName);
  else if (!matcher.m_defaultName.empty())
    name.swap(matcher.m_defaultName);
  else if (!matcher.m_intName.empty())
    name.swap(matcher.m_intName);
  else
    name.swap(matcher.m_englishName);
}

string FeatureType::GetHouseNumber() const
{
  ParseCommon();
  return m_params.house.Get();
}

bool FeatureType::GetName(int8_t lang, string & name) const
{
  if (!HasName())
    return false;

  ParseCommon();
  return m_params.name.GetString(lang, name);
}

uint8_t FeatureType::GetRank() const
{
  ParseCommon();
  return m_params.rank;
}

uint32_t FeatureType::GetPopulation() const
{
  uint8_t const r = GetRank();
  return (r == 0 ? 1 : static_cast<uint32_t>(pow(1.1, r)));
}

string FeatureType::GetRoadNumber() const
{
  ParseCommon();
  return m_params.ref;
}

bool FeatureType::HasInternet() const
{
  ParseTypes();

  bool res = false;

  ForEachType([&res](uint32_t type)
  {
    if (!res)
    {
      static const uint32_t t1 = classif().GetTypeByPath({"internet_access"});

      ftype::TruncValue(type, 1);
      res = (type == t1);
    }
  });

  return res;
}

void FeatureType::SwapGeometry(FeatureType & r)
{
  ASSERT_EQUAL(m_bPointsParsed, r.m_bPointsParsed, ());
  ASSERT_EQUAL(m_bTrianglesParsed, r.m_bTrianglesParsed, ());

  if (m_bPointsParsed)
    m_points.swap(r.m_points);

  if (m_bTrianglesParsed)
    m_triangles.swap(r.m_triangles);
}
