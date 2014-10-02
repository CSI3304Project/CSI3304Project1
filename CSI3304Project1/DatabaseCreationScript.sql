DROP DATABASE ImageBaseDataBase;
CREATE DATABASE ImageBaseDataBase;
USE ImageBaseDataBase;
CREATE TABLE tblUser
(	userUsername varchar(20) NOT NULL,
	userPassword varchar(50) NOT NULL,
	userFirstName varchar(30) NOT NULL,
	userLastName varchar(30) NOT NULL,
	userEmail varchar(64) NOT NULL,
	userType varchar(10) NOT NULL,
	PRIMARY KEY (userUsername)
);

CREATE TABLE tblImage
(
	imgImageID int NOT  NULL,
	imgImageName varchar(30) NOT NULL,
	imgUploadDate date NOT NULL,
	imgProvider varchar(20) NOT NULL,
	imgTags varchar(200) NOT NULL,
	imgStatus varchar(10) NOT NULL,
	imgImageFile blob NOT NULL,
	imgImageType varchar(10) NOT NULL,
	PRIMARY KEY (imgImageID)
);

CREATE TABLE tblDownloads
(
	dnldImageID int NOT NULL,
	dnldUsername varchar(20) NOT NULL,
	dnldDownloadTime datetime NOT NULL,
	FOREIGN KEY (dnldImageID) REFERENCES tblImage(imgImageID),
	FOREIGN KEY (dnldUsername) REFERENCES tblUser(userUsername)
);

CREATE TABLE tblLogs
(
	logUsername varchar(20) NOT NULL,
	logUserType varchar(10) NOT NULL,
	logTime datetime NOT NULL,
	FOREIGN KEY (logUsername) REFERENCES tblUser(userUsername)
);