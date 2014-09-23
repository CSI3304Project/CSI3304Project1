using System;
using System.Collections;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Drawing;

namespace CSI3304Project1
{
    public class UploadedImage
    {
        private int imageID;
        private DateTime imageUploadDate;
        private string imageProvider;
        private ArrayList imageTags;
        private string imageStatus;
        private Image imageFile;

        public UploadedImage()
        {
            imageID = 0;
            imageUploadDate = new DateTime(2001, 01, 01);
            imageProvider = "Username01";
            imageTags = new ArrayList();
            imageTags.Add("tag");
            imageStatus = "Unmoderated";
        }

        public void setImageID(int inputID)
        {
            imageID = inputID;
        }

        public void setImageUploadDate(DateTime inputDate)
        {
            imageUploadDate = inputDate;
        }

        public void setImageProvider(string inputProvider)
        {
            imageProvider = inputProvider;
        }

        public void setImageTags(ArrayList inputTags)
        {
            imageTags = inputTags;
        }

        public void setImageStatus(string inputStatus)
        {
            imageStatus = inputStatus;
        }

        public int getImageID()
        {
            return imageID;
        }

        public DateTime getImageUploadDate()
        {
            return imageUploadDate;
        }

        public string getImageProvider()
        {
            return imageProvider;
        }

        public ArrayList getImageTags()
        {
            return imageTags;
        }

        public string getImageStatus()
        {
            return imageStatus;
        }
    }
}
