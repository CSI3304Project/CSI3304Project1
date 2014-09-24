using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Drawing;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;
using System.Data.SqlClient;

namespace CSI3304Project1
{
    public partial class Login : Form
    {
        public Login()
        {
            InitializeComponent();
        }

        private void button1_Click(object sender, EventArgs e)
        {
            //Assign text box fields
            string enteredUsername = txtUserName.Text;
            string enteredPassword = txtPassword.Text;
            //Connect to database. Need to test
            string connetionString = null;
            SqlConnection cnn ;
            connetionString = "Data Source=ServerName;Initial Catalog=DatabaseName;User ID=UserName;Password=Password";
            cnn = new SqlConnection(connetionString);
            try
            {
                cnn.Open();
                //Look for match
                MessageBox.Show("Connection made");
                string CommandText = "SELECT * FROM #####";
                //Stuck here
                cnn.Close();
            }
            catch
            {
                MessageBox.Show("Unable to open connection to database.");
            }
        }
    }

}
