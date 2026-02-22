<!-- PROJECT SHIELDS -->
[![Contributors][contributors-shield]][contributors-url]
[![Forks][forks-shield]][forks-url]
[![Stargazers][stars-shield]][stars-url]
[![Issues][issues-shield]][issues-url]

<!-- PROJECT LOGO -->
<br />
<p align="center">
  <a href="https://github.com/xnaval/SQLiteExtensions">
    SQLiteExtensions
  </a>

  <h3 align="center">SQLiteExtensions</h3>

  <p align="center">
    An "SQLite extension" that implements the minimum functions to create and handle a GeoPackage versions 1.2, 1.3 and 1.4
    <br />
    <a href="https://github.com/xnaval/SQLiteExtensions"><strong>Explore the docs »</strong></a>
    <br />
    <br />
    <a href="https://github.com/xnaval/SQLiteExtensions/issues">Report Bug</a>
    ·
    <a href="https://github.com/xnaval/SQLiteExtensions/issues">Request Feature</a>
  </p>
</p>

<!-- TABLE OF CONTENTS -->
<details open="open">
  <summary><h2 style="display: inline-block">Table of Contents</h2></summary>
  <ol>
    <li>
      <a href="#about-the-project">About The Project</a>
      <ul>
        <li><a href="#built-with">Built With</a></li>
      </ul>
    </li>
    <li>
      <a href="#getting-started">Getting Started</a>
      <ul>
        <li><a href="#prerequisites">Prerequisites</a></li>
        <li><a href="#installation">Installation</a></li>
      </ul>
    </li>
    <li><a href="#usage">Usage</a></li>
    <li><a href="#contributing">Contributing</a></li>
    <li><a href="#contact">Contact</a></li>
    <li><a href="#acknowledgements">Acknowledgements</a></li>
  </ol>
</details>

<!-- ABOUT THE PROJECT -->
## About The Project

This is an "SQLite extension" that implements the minimum functions to create and handle a GeoPackage version 1.2, 1.3 and 1.4.

By the moment it only handles the vector features.

### Built With

* [VisualStudio](https://visualstudio.microsoft.com/)
* [c]

<!-- GETTING STARTED -->
## Getting Started

To get a local copy up and running follow these simple steps.

### Prerequisites

Open the solution (SQLiteExtensions.sln) with VisualStudio 2022 and compile it.

### Installation

- Copy the file ```gpkg.dll``` to a known folder, for example ```c:\SQlite```
   - If you need the 32bit version, the file gpkg.dll is located at ```\SQLiteExtensions\Win32\Release\gpkg.dll```
   - If you need the 64bit version, the file gpkg.dll is located at ```\SQLiteExtensions\x64\Release\gpkg.dll```

<!-- USAGE EXAMPLES -->
## Usage

* open a GeoPackage with ```sqlite3.exe```
```
sqlite3
.open sample.gpkg -- Open the sample GeoPackage
.load C:\\SQLite\\gpkg.dll sqlite3_gpkg_init -- Load the extension
```
* To initalize a GeoPackage, that is, create the base tables for an empty GeoPackage
```
select GPKG_Initialize();
```
This will create an 1.4 GeoPackage.
To create an 1.3 geoPackage do
```
select GPKG_Initialize(10300);
```
* To register an existing table with a geometry column
```
select GPKG_AddGeometryColumn(identifier, tableName, geometryColumn, geometryType, srsId, zFlag, mFlag); 
```
   + ```identifier``` -> Identifier of the geometry (gpkg_contents)
   + ```tableName``` -> Name of the table
   + ```geometryColumn``` -> Column that contains the geometry
   + ```geometryType``` -> WKT geometry name : "GEOMETRY", "POINT", "LINESTRING", "POLYGON", "MULTIPOINT", "MULTILINESTRING", "MULTIPOLYGON", "GEOMCOLLECTION"
   + ```srsId``` -> SRS ID of the geometries
   + ```zFlag``` -> 0: z values prohibited; 1: z values mandatory; 2: z values optional
   + ```mflag``` -> 0: m values prohibited; 1: m values mandatory; 2: m values optional

   This function populates the ```gpkg_contents table``` (if not already present) and also populates the ```gpkg_geometry_columns table```.

* To add a spatial index to a geometry table
```
select GPKG_AddSpatialIndex(tableName, geometryColumn, idColumn);
```
   + ```tableName``` -> Name of the table
   + ```geometryColumn``` -> Column that contains the geometry
   + ```idColumn``` -> Primary key of the table

   This function creates a spatial index of a table and the corresponding triggers to maintain the integrity between the spatial index and the table, also registers the gpkg extension ```gpkg_rtree_index``` and populates the spatial index.

* To drop a spatial index of a geometry table
```
select GPKG_DropSpatialIndex(tableName, geometryColumn, idColumn);
```
   + ```tableName``` -> Name of the table
   + ```geometryColumn``` -> Column that contains the geometry
   + ```idColumn``` -> Primary key of the table

   This function Drops a spatial index of a table and the corresponding triggers to maintain the integrity between the spatial index and the table, and also unregisters the gpkg extension ```gpkg_rtree_index```.

* Other functions implemented are
   + ```select GPKG_ExtVersion();``` -> Returns an string showing the version of this extension.
   + ```select GPKG_Version();``` -> Returns an integer showing the GeoPackage version.
   + ```select ST_MinX(geometry);``` -> Returns the minimum X of a geometry or NULL if there is an error.
   + ```select ST_MinY(geometry);``` -> Returns the minimum Y of a geometry or NULL if there is an error.
   + ```select ST_MinZ(geometry);``` -> Returns the minimum Z of a geometry or NULL if there is an error.
   + ```select ST_MinM(geometry);``` -> Returns the minimum M of a geometry or NULL if there is an error.
   + ```select ST_MaxX(geometry);``` -> Returns the maximum X of a geometry or NULL if there is an error.
   + ```select ST_MaxY(geometry);``` -> Returns the maximum Y of a geometry or NULL if there is an error.
   + ```select ST_MaxZ(geometry);``` -> Returns the maximum Z of a geometry or NULL if there is an error.
   + ```select ST_MaxM(geometry);``` -> Returns the maximum M of a geometry or NULL if there is an error.
   + ```select ST_IsEmpty(geometry);``` -> Returns 1 if the geometry is empty, 0 if it is not empty, -1 if there is an error (therefore ISEMPTY (GEOM) is evaluated to TRUE).
   
<!-- CONTRIBUTING -->
## Contributing

Contributions are what make the open source community such an amazing place to be learn, inspire, and create. Any contributions you make are **greatly appreciated**.

1. Fork the Project
2. Create your Feature Branch (`git checkout -b feature/AmazingFeature`)
3. Commit your Changes (`git commit -m 'Add some AmazingFeature'`)
4. Push to the Branch (`git push origin feature/AmazingFeature`)
5. Open a Pull Request

<!-- CONTACT -->
## Contact

Your Name - [@@xnaval](https://twitter.com/@xnaval) - xnaval@telefonica.net

Project Link: [https://github.com/xnaval/SQLiteExtensions](https://github.com/xnaval/SQLiteExtensions)

<!-- ACKNOWLEDGEMENTS -->
## Acknowledgements

* [GeoPackage](https://www.geopackage.org/)
* [SpatiaLite](https://www.gaia-gis.it/fossil/libspatialite/index)
* [Best-README-Template](https://github.com/othneildrew/Best-README-Template)

<!-- MARKDOWN LINKS & IMAGES -->
<!-- https://www.markdownguide.org/basic-syntax/#reference-style-links -->
[contributors-shield]: https://img.shields.io/github/contributors/xnaval/SQLiteExtensions.svg?style=for-the-badge
[contributors-url]: https://github.com/xnaval/SQLiteExtensions/graphs/contributors
[forks-shield]: https://img.shields.io/github/forks/xnaval/SQLiteExtensions.svg?style=for-the-badge
[forks-url]: https://github.com/xnaval/SQLiteExtensions/network/members
[stars-shield]: https://img.shields.io/github/stars/xnaval/SQLiteExtensions.svg?style=for-the-badge
[stars-url]: https://github.com/xnaval/SQLiteExtensions/stargazers
[issues-shield]: https://img.shields.io/github/issues/xnaval/SQLiteExtensions.svg?style=for-the-badge
[issues-url]: https://github.com/xnaval/SQLiteExtensions/issues
