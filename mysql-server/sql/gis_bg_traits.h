#ifndef GIS_BG_TRAITS_INCLUDED
#define GIS_BG_TRAITS_INCLUDED

/* Copyright (c) 2014, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA
*/


/* This file defines all boost geometry traits. */

#include <boost/mpl/int.hpp>
#include <boost/static_assert.hpp>

#include <boost/geometry/core/access.hpp>
#include <boost/geometry/core/coordinate_type.hpp>
#include <boost/geometry/core/coordinate_system.hpp>
#include <boost/geometry/core/coordinate_dimension.hpp>
#include <boost/geometry/core/interior_type.hpp>
#include <boost/geometry/core/ring_type.hpp>
#include <boost/geometry/core/exterior_ring.hpp>
#include <boost/geometry/core/interior_rings.hpp>
#include <boost/geometry/core/point_order.hpp>
#include <boost/geometry/core/closure.hpp>
#include <boost/geometry/core/tags.hpp>
#include <boost/geometry/core/cs.hpp>
#include <boost/geometry/multi/core/tags.hpp>
#include <boost/geometry/util/math.hpp>
#include <boost/concept/requires.hpp>

#include <boost/geometry/geometries/concepts/point_concept.hpp>
#include <boost/geometry/geometries/concepts/linestring_concept.hpp>
#include <boost/geometry/geometries/concepts/polygon_concept.hpp>

#include "spatial.h"


// Boost Geometry traits.
namespace boost { namespace geometry
{
namespace traits
{
template<>
struct tag<Gis_point>
{
  typedef boost::geometry::point_tag type;
};

template<>
struct coordinate_type<Gis_point>
{
  typedef double type;
};

template<>
struct coordinate_system<Gis_point>
{
  typedef boost::geometry::cs::cartesian type;
};

template<>
struct dimension<Gis_point>
  : boost::mpl::int_<GEOM_DIM>
{};

template<std::size_t Dimension>
struct access<Gis_point, Dimension>
{
  static inline double get(
    Gis_point const& p)
  {
    return p.get<Dimension>();
  }

  static inline void set(
    Gis_point &p,
    double const& value)
  {
    p.set<Dimension>(value);
  }
};
////////////////////////////////// LINESTRING ////////////////////////////
template<>
struct tag<Gis_line_string>
{
  typedef boost::geometry::linestring_tag type;
};
////////////////////////////////// POLYGON //////////////////////////////////


template<>
struct tag
<
  Gis_polygon
>
{
  typedef boost::geometry::polygon_tag type;
};

template<>
struct ring_const_type
<
  Gis_polygon
>
{
  typedef Gis_polygon::ring_type const& type;
};


template<>
struct ring_mutable_type
<
  Gis_polygon
>
{
  typedef Gis_polygon::ring_type& type;
};

template<>
struct interior_const_type
<
  Gis_polygon
>
{
  typedef Gis_polygon::inner_container_type const& type;
};


template<>
struct interior_mutable_type
<
  Gis_polygon
>
{
  typedef Gis_polygon::inner_container_type& type;
};

template<>
struct exterior_ring
<
  Gis_polygon
>
{
  typedef Gis_polygon polygon_type;

  static inline polygon_type::ring_type& get(polygon_type& p)
  {
    return p.outer();
  }

  static inline polygon_type::ring_type const& get(
          polygon_type const& p)
  {
    return p.outer();
  }
};

template<>
struct interior_rings
<
  Gis_polygon
>
{
  typedef Gis_polygon polygon_type;

  static inline polygon_type::inner_container_type& get(
          polygon_type& p)
  {
    return p.inners();
  }

  static inline polygon_type::inner_container_type const& get(
          polygon_type const& p)
  {
    return p.inners();
  }
};

////////////////////////////////// RING //////////////////////////////////
template<>
struct point_order<Gis_polygon_ring>
{
  static const order_selector value = counterclockwise;
};

template<>
struct closure<Gis_polygon_ring>
{
  static const closure_selector value = closed;
};

template<>
struct tag<Gis_polygon_ring>
{
  typedef boost::geometry::ring_tag type;
};
////////////////////////////////// MULTI GEOMETRIES /////////////////////////
template<>
struct tag< Gis_multi_line_string>
{
  typedef boost::geometry::multi_linestring_tag type;
};

template<>
struct tag< Gis_multi_point>
{
  typedef boost::geometry::multi_point_tag type;
};

template<>
struct tag< Gis_multi_polygon>
{
  typedef boost::geometry::multi_polygon_tag type;
};

} // namespace traits

}} // namespace boost::geometry


#endif // !GIS_BG_TRAITS_INCLUDED
